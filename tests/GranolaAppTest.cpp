// Granola in score, its panel used as a user would while it plays: sounds and
// folders dropped, the waveform dragged, controls changed, undone and redone,
// the panel rebuilt and deleted, the process torn down mid-playback. The
// executor runs as the audio graph runs it: real tokens, the execution queue
// drained as the audio thread. Nothing may crash; the outputs stay finite.
#include "Granola/Granola.hpp"

#include <ossia/math/safe_math.hpp>

#include <Process/Commands/SetControlValue.hpp>
#include <Process/ProcessList.hpp>
#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortItem.hpp>
#include <Process/Preset.hpp>
#include <Process/DocumentPlugin.hpp>
#include <Process/Focus/FocusDispatcher.hpp>
#include <Process/LayerPresenter.hpp>
#include <Process/ProcessContext.hpp>

#include <Dataflow/Commands/EditConnection.hpp>
#include <Process/Dataflow/Cable.hpp>
#include <Scenario/Document/Interval/IntervalModel.hpp>
#include <Scenario/Document/ScenarioDocument/ScenarioDocumentModel.hpp>

#include <Execution/DocumentPlugin.hpp>

#include <score/command/Dispatchers/CommandDispatcher.hpp>
#include <score/graphics/GraphicsLayout.hpp>

#include <core/command/CommandStack.hpp>
#include <core/document/Document.hpp>
#include <core/document/DocumentModel.hpp>

#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/graph_node.hpp>
#include <ossia/dataflow/port.hpp>
#include <ossia/dataflow/token_request.hpp>
#include <ossia/detail/thread.hpp>

#include <QApplication>
#include <QDialog>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <Avnd/Factories.hpp>
#include <catch2/catch_all.hpp>
#include <score_test/App.hpp>
#include <score_test/Document.hpp>
#include <score_test/Project.hpp>

#include <cmath>
#include <fstream>
#include <map>
#include <thread>

namespace
{
using G = Granola::Granola;

//! A tick of \p buffer samples from \p at, the transport moving at \p tempo
//! from \p quarters on, in 4/4
ossia::token_request
make_token(int64_t& at, int rate, int buffer, double tempo, double& quarters)
{
  const int64_t per_sample = 705600000 / rate;
  ossia::token_request tk;
  tk.prev_date = ossia::time_value{per_sample * at};
  at += buffer;
  tk.date = ossia::time_value{per_sample * at};
  tk.parent_duration = ossia::time_value{per_sample * 48000 * 600};
  tk.speed = 1.;
  tk.tempo = tempo;
  tk.signature = ossia::time_signature{4, 4};
  tk.start_sample = 0;
  tk.length_sample = buffer;
  tk.musical_start_position = quarters;
  quarters += buffer * tempo / 60. / rate;
  tk.musical_end_position = quarters;
  tk.musical_start_last_bar = std::floor(tk.musical_start_position / 4. + 1e-9) * 4.;
  tk.musical_end_last_bar = std::floor(tk.musical_end_position / 4. + 1e-9) * 4.;
  return tk;
}

void run_exec(Execution::DocumentPlugin& plug)
{
  ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
  plug.runAllCommands();
  ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
}

void drain_gc(Execution::DocumentPlugin& plug)
{
  auto& data = *plug.contextData();
  Execution::GCCommand gc;
  Execution::ExecutionCommand cmd;
  for(bool any = true; any;)
  {
    any = false;
    if(data.m_editionQueue.try_dequeue(cmd))
    {
      cmd();
      any = true;
    }
    if(data.m_gcQueue.try_dequeue(gc))
    {
      gc();
      any = true;
    }
  }
}

void spin(int ms = 0)
{
  QElapsedTimer t;
  t.start();
  do
    QApplication::processEvents(QEventLoop::AllEvents, 5);
  while(t.elapsed() < ms);
}

void write_wav(const QString& path, double freq, int frames, int channels)
{
  std::ofstream f(path.toStdString(), std::ios::binary);
  auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
  auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
  const int rate = 48000;
  f.write("RIFF", 4);
  w32(36 + frames * channels * 2);
  f.write("WAVEfmt ", 8);
  w32(16);
  w16(1);
  w16(uint16_t(channels));
  w32(rate);
  w32(rate * channels * 2);
  w16(uint16_t(channels * 2));
  w16(16);
  f.write("data", 4);
  w32(frames * channels * 2);
  for(int i = 0; i < frames; i++)
    for(int c = 0; c < channels; c++)
      w16(uint16_t(int16_t(12000 * std::sin(2 * M_PI * freq * (c + 1) * i / rate))));
}

//! Granola as an interval component mounts it, with its panel in a view
struct Mounted
{
  score::Document& doc;
  Execution::DocumentPlugin& plug;
  oscr::ProcessModel<G>* model{};
  bool owns_model{true};
  std::shared_ptr<oscr::Executor<G>> comp;
  int64_t at{};
  // The transport: its tempo, and where it is, in quarter notes
  double tempo{120.};
  double quarters{};

  Process::DataflowManager dfm;
  FocusDispatcher fd;
  Process::Context pctx;
  QGraphicsScene scene;
  QGraphicsView view{&scene};
  QGraphicsRectItem root{QRectF{0., 0., 3000., 3000.}};
  oscr::LayerFactory<G> layers;
  QGraphicsItem* panel{};
  bool finite{true};
  // A dialog a click opens, e.g. on the waveform's name, waits for an answer
  // no test gives: dismissed, as one would
  QTimer dismiss;
  int dismissed{};

  //! A new Granola, or \p existing, one of the document
  Mounted(
      score::Document& d, Execution::DocumentPlugin& p,
      oscr::ProcessModel<G>* existing = nullptr)
      : doc{d}
      , plug{p}
      , pctx{d.context(), dfm, fd}
  {
    if(existing)
    {
      model = existing;
      owns_model = false;
    }
    else
      model = new oscr::ProcessModel<G>{
          TimeVal::fromMsecs(600000), Id<Process::ProcessModel>{9301}, doc.context(),
          &score::test::base_interval(doc)};
    comp = std::make_shared<oscr::Executor<G>>(*model, plug.context(), nullptr);
    REQUIRE(comp->node);
    plug.context().setup.register_node(*model, comp->node);
    run_exec(plug);

    dismiss.setInterval(30);
    QObject::connect(&dismiss, &QTimer::timeout, [this] {
      if(auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
      {
        d->reject();
        dismissed++;
      }
    });
    dismiss.start();

    view.resize(2000, 1200);
    view.show();
    scene.addItem(&root);
    build_panel();
  }

  ~Mounted()
  {
    delete_panel();
    teardown();
    if(owns_model)
      delete model;
    spin();
    drain_gc(plug);
    scene.removeItem(&root);
  }

  void build_panel()
  {
    Process::LayerFactory& f = layers;
    panel = f.makeItem(*model, pctx, &root);
    REQUIRE(panel);
    spin();
  }
  void delete_panel()
  {
    delete panel;
    panel = nullptr;
    spin();
  }
  void teardown()
  {
    if(!comp)
      return;
    comp->cleanup();
    run_exec(plug);
    comp.reset();
    drain_gc(plug);
  }

  G& object()
  {
    return static_cast<oscr::safe_node<G>&>(*comp->node).impl.effect;
  }

  //! A block as the graph runs it, then the UI's turn
  void tick(int buffer = 512)
  {
    if(!comp)
      return;
    auto& n = *comp->node;
    for(auto* o : n.root_outputs())
      if(auto* a = o->target<ossia::audio_port>())
        for(auto& chan : *a)
          chan.clear();
    ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
    plug.runAllCommands();
    n.begin_execution();
    n.run(
        make_token(at, 48000, buffer, tempo, quarters),
        ossia::exec_state_facade{plug.context().execState.get()});
    log.after_tick(object(), at, buffer);
    for(auto* i : n.root_inputs())
      if(auto* v = i->target<ossia::value_port>())
        v->get_data().clear();
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    for(auto* o : n.root_outputs())
      if(auto* a = o->target<ossia::audio_port>())
        for(auto& chan : *a)
          for(double x : chan)
            finite &= ossia::safe_isfinite(x);
    QApplication::processEvents();
  }
  void play(int blocks, int buffer = 512)
  {
    for(int k = 0; k < blocks; k++)
      tick(k % 7 == 3 ? 64 + (k * 97) % 2000 : buffer);
  }
  template <typename F>
  bool play_until(F pred, int ms = 8000)
  {
    QElapsedTimer t;
    t.start();
    while(t.elapsed() < ms)
    {
      tick();
      if(pred())
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
  }

  Process::ControlInlet& inlet(const QString& name)
  {
    for(auto* i : model->inlets())
      if(i->name() == name)
        if(auto* c = qobject_cast<Process::ControlInlet*>(i))
          return *c;
    FAIL("no inlet " << name.toStdString());
    throw;
  }
  //! As the inspector or a widget does: a command
  void set(const QString& name, ossia::value v)
  {
    CommandDispatcher<>{doc.context().commandStack}.submit<Process::SetControlValue>(
        inlet(name), std::move(v));
    spin();
  }

  // The waveform: the item of the panel that fills its width
  QGraphicsItem* waveform()
  {
    for(auto* it : scene.items())
      if(dynamic_cast<score::FillsWidth*>(it) && it->boundingRect().height() == 160.)
        return it;
    return nullptr;
  }
  QPoint at_waveform(double fx, double y)
  {
    auto* w = waveform();
    REQUIRE(w);
    const auto r = w->sceneBoundingRect();
    return view.mapFromScene(r.left() + 2. + fx * (r.width() - 4.), r.top() + y);
  }
  void mouse(QEvent::Type t, QPoint p, Qt::MouseButtons b)
  {
    QMouseEvent e(t, p, view.viewport()->mapToGlobal(p), Qt::LeftButton, b, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &e);
  }
  //! A drag on the waveform, ticking as it goes
  void drag(QPoint from, QPoint to)
  {
    mouse(QEvent::MouseButtonPress, from, Qt::LeftButton);
    for(int k = 1; k <= 8; k++)
    {
      mouse(QEvent::MouseMove, from + (to - from) * k / 8, Qt::LeftButton);
      tick(256);
    }
    mouse(QEvent::MouseButtonRelease, to, Qt::NoButton);
    spin(20);
  }
  void drop(const QString& path, QPoint p)
  {
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    QDragEnterEvent enter(p, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &enter);
    QDragMoveEvent move(p, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &move);
    QDropEvent drop(p, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &drop);
    spin(50);
  }
  bool playing_grains()
  {
    return std::ranges::any_of(object().grains, [](auto& g) { return g.m_active; });
  }

  //! The grains made, as the ticks show them: when each started, and how
  //! long it played once it is over: more than the samples it had played by
  //! the last tick it was seen in, at most a tick more
  struct GrainLog
  {
    struct grain
    {
      int64_t start{};
      int64_t played{-1}; // -1 while it plays
      int64_t within{};   // the frames of the tick it ended in
      //! Whether it lasted \p samples
      bool lasted(double samples) const noexcept
      {
        return played >= 0 && double(played) < samples + 1.
               && samples <= double(played + within) + 1.;
      }
    };
    static constexpr int64_t unknown = -2;
    std::vector<grain> grains;
    std::vector<long> of_voice;   // its grain's index in grains, -1 none
    std::vector<long> last_count; // its grain's phase counter, last tick
    void after_tick(const G& g, int64_t now, int frames)
    {
      const auto n = g.grains.size();
      of_voice.resize(n, -1);
      last_count.resize(n, -1);
      for(std::size_t i = 0; i < n; i++)
      {
        const auto& gr = g.grains[i];
        const long c = gr.m_phase_counter;
        // Another grain in the voice since the last tick: it started c ago
        const bool fresh = gr.m_active && (of_voice[i] < 0 || c < last_count[i]
                                           || grains[of_voice[i]].played != -1);
        if(fresh)
        {
          // Its last one ended and another started within a tick: its
          // length is not known
          if(of_voice[i] >= 0 && grains[of_voice[i]].played < 0)
            grains[of_voice[i]].played = unknown;
          of_voice[i] = (long)grains.size();
          grains.push_back({now - c, -1, 0});
        }
        else if(!gr.m_active && of_voice[i] >= 0 && grains[of_voice[i]].played < 0)
        {
          // Over within this tick
          grains[of_voice[i]].played = last_count[i];
          grains[of_voice[i]].within = frames;
        }
        last_count[i] = c;
      }
    }
    //! The grains over, started from \p from on
    std::vector<grain> over(int64_t from = 0) const
    {
      std::vector<grain> res;
      for(auto& g : grains)
        if(g.played >= 0 && g.start >= from)
          res.push_back(g);
      std::ranges::sort(res, {}, &grain::start);
      return res;
    }
    std::vector<int64_t> starts(int64_t from = 0) const
    {
      std::vector<int64_t> res;
      for(auto& g : grains)
        if(g.start >= from)
          res.push_back(g.start);
      std::ranges::sort(res);
      return res;
    }
  } log;
};

Execution::DocumentPlugin& load_execution(score::Document& doc)
{
  auto& plug = doc.context().plugin<Execution::DocumentPlugin>();
  plug.reload(true, score::test::base_interval(doc));
  run_exec(plug);
  REQUIRE(plug.context().execState);
  return plug;
}
}

TEST_CASE("A sound dropped, played, dragged, changed, undone, redone", "[app]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QTemporaryDir dir;
    write_wav(dir.filePath("a.wav"), 220, 48000, 1);
    write_wav(dir.filePath("b.wav"), 330, 24000, 2);
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    auto& stack = doc->commandStack();
    {
      Mounted m{*doc, plug};
      m.play(20); // no sound yet

      // A sound dropped on the waveform, as one
      m.drop(dir.filePath("a.wav"), m.at_waveform(0.5, 80.));
      REQUIRE(m.play_until([&] { return m.playing_grains(); }));
      CHECK(m.finite);

      // The window, its edges and the region's handles dragged, playing;
      // the handles once the waveform is drawn
      m.drag(m.at_waveform(0.2, 80.), m.at_waveform(0.6, 60.));
      m.play(200);
      m.drag(m.at_waveform(0.0, 5.), m.at_waveform(0.3, 5.));  // region start
      m.drag(m.at_waveform(1.0, 5.), m.at_waveform(0.25, 5.)); // end past start
      m.drag(m.at_waveform(0.27, 5.), m.at_waveform(1.0, 5.));

      // Every control that changes what is made, playing
      for(int v : {1, 256, 0, 64})
      {
        m.set("Max Voices", v);
        m.play(5);
      }
      for(int n : {16, 100, 2, 0})
      {
        m.set("Output Channels", n);
        m.play(5);
      }
      for(float s : {1.f, 2.f, 0.f})
      {
        m.set("Spread", s);
        m.play(5);
      }
      m.set("Source Channels", 8);
      m.set("Channel Offset", 7);
      m.play(5);
      m.set("Interpolation mode", std::string("None"));
      m.set("Window mode", std::string("Kuma"));
      m.set("Loop", true);
      m.set("Scan", 2.f);
      m.set("Scan Mode", std::string("PingPong"));
      m.set("Sample Start", 1.f);
      m.set("Sample End", 1.f);
      m.play(10);
      m.set("Duration", 0.f);
      m.set("Density", 256.f);
      m.set("Pitch", 10.f);
      m.play(10);

      // A click on the waveform's name opens a dialog: the panel is rebuilt
      // and the process plays while it is open
      QTimer::singleShot(10, [&] { m.delete_panel(); m.build_panel(); });
      const auto p = m.at_waveform(0.5, 5.);
      m.mouse(QEvent::MouseButtonPress, p, Qt::LeftButton);
      m.mouse(QEvent::MouseButtonRelease, p, Qt::NoButton);
      m.play(10);
      spin(200);

      // Another sound, stereo and shorter, while grains read the first
      m.drop(dir.filePath("b.wav"), m.at_waveform(0.5, 80.));
      m.play(30);

      // Undone and redone, all of it, playing
      const int done = stack.currentIndex();
      while(stack.canUndo())
      {
        stack.undoQuiet();
        m.play(2);
      }
      while(stack.currentIndex() < done && stack.canRedo())
      {
        stack.redoQuiet();
        m.play(2);
      }

      // The panel rebuilt while grains play and messages come
      m.delete_panel();
      m.play(10);
      m.build_panel();
      m.play(10);
      CHECK(m.finite);
    } // torn down while grains play
  });
}

TEST_CASE("A folder dropped, played at random, changed on disk while it plays", "[app]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QTemporaryDir dir, other;
    for(int k = 0; k < 6; k++)
      write_wav(dir.filePath(QString("s%1.wav").arg(k)), 200 + 50 * k, 4800 * (k + 1), 1 + k % 2);
    write_wav(other.filePath("x.wav"), 500, 2000, 1);
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    Mounted m{*doc, plug};

    m.drop(dir.path(), m.at_waveform(0.5, 80.));
    REQUIRE(m.play_until([&] { return m.playing_grains(); }));

    m.set("Sound Order", std::string("Random"));
    m.set("Scan", 1.f);
    m.set("Scan in folder", std::string("Per file"));
    m.play(40);

    // Files removed and added while grains read them
    QFile::remove(dir.filePath("s0.wav"));
    QFile::remove(dir.filePath("s3.wav"));
    write_wav(dir.filePath("s9.wav"), 900, 1000, 2);
    m.play(100);

    // A number for the file: an index, any
    for(std::string v : {"2", "1e9", "-3", "x.wav", "s9.wav"})
    {
      m.set("Sound in folder", v);
      m.play(5);
    }
    // Another folder, then back to the file
    m.drop(other.path(), m.at_waveform(0.5, 80.));
    m.play(40);
    m.set("Source", std::string("File"));
    m.play(20);
    CHECK(m.finite);
  });
}

TEST_CASE("The panel and the process gone in any order, mid-playback", "[app]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QTemporaryDir dir;
    write_wav(dir.filePath("a.wav"), 220, 48000, 1);
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    for(int order = 0; order < 3; order++)
    {
      CAPTURE(order);
      Mounted m{*doc, plug};
      m.drop(dir.filePath("a.wav"), m.at_waveform(0.5, 80.));
      REQUIRE(m.play_until([&] { return m.playing_grains(); }));
      m.set("Density", 200.f);
      m.play(5);
      switch(order)
      {
        case 0: // the panel first, messages still to it
          m.delete_panel();
          m.play(5);
          m.teardown();
          break;
        case 1: // the process first, the panel left
          m.teardown();
          spin(50);
          m.delete_panel();
          break;
        case 2: // the panel rebuilt over and over
          for(int k = 0; k < 5; k++)
          {
            m.delete_panel();
            m.build_panel();
            m.play(3);
          }
          break;
      }
      CHECK(m.finite);
    }
  });
}

namespace
{
const QString granola_v1_uuid = QStringLiteral("38F9684D-54A6-4F48-91E4-3B251F0956EA");

Process::ProcessModel* add_by_uuid(score::Document& doc, const QString& uuid)
{
  return score::test::add_process(doc, uuid, {});
}

template <typename Ports>
auto port_named(const Ports& ports, const QString& name)
{
  for(auto* p : ports)
    if(p->name() == name)
      return p;
  return decltype(*ports.begin()){};
}

std::vector<Process::ProcessModel*> processes_of(score::Document& doc, const QString& uuid)
{
  std::vector<Process::ProcessModel*> found;
  for(auto& p : score::test::base_interval(doc).processes)
    if(p.concreteKey() == UuidKey<Process::ProcessModel>::fromString(uuid))
      found.push_back(&p);
  return found;
}

Scenario::ScenarioDocumentModel& scenario_model(score::Document& doc)
{
  return static_cast<Scenario::ScenarioDocumentModel&>(doc.model().modelDelegate());
}
}

TEST_CASE("A document of the Granola released first loads with its ports, values and cables", "[app][v1]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    // The uuid the documents of the first Granola name: what score made then
    auto* granola = add_by_uuid(*doc, granola_v1_uuid);
    auto* other = add_by_uuid(*doc, granola_v1_uuid);
    REQUIRE(granola);
    REQUIRE(other);
    QStringList names;
    for(auto* i : granola->inlets())
      names.push_back(i->name());
    CHECK(
        names
        == QStringList{
            "Sound", "Position", "Position Jitter", "Position Jitter Range", "Duration",
            "Duration Jitter", "Duration Jitter Range", "Pitch", "Pitch Jitter",
            "Pitch Jitter Range", "Reverse", "Density", "Density Jitter",
            "Density Jitter Range", "Gain", "Gain Jitter", "Gain Jitter Range",
            "Window coefs", "Interpolation mode", "Window mode", "Loop", "Source Channels",
            "Channel Offset", "Max Voices", "Continuous", "Trigger", "MIDI In",
            "listening to MIDI"});

    // Values on the ports the new Granola has not, and a cable into one
    const std::vector<std::pair<QString, float>> values{
        {"Position Jitter", 0.4f},       {"Position Jitter Range", 0.3f},
        {"Duration Jitter Range", 0.6f}, {"Density Jitter Range", 12.f},
        {"Gain Jitter Range", 0.25f},    {"Density", 7.f}};
    for(auto& [name, v] : values)
    {
      auto* inlet = qobject_cast<Process::ControlInlet*>(port_named(granola->inlets(), name));
      REQUIRE(inlet);
      CommandDispatcher<>{doc->context().commandStack}.submit<Process::SetControlValue>(
          *inlet, v);
    }
    auto* from = port_named(other->outlets(), "Active Grains");
    auto* to = port_named(granola->inlets(), "Pitch Jitter Range");
    REQUIRE(from);
    REQUIRE(to);
    CommandDispatcher<>{doc->context().commandStack}.submit(new Dataflow::CreateCable{
        scenario_model(*doc), Id<Process::Cable>{4243}, Process::CableType::ImmediateGlutton,
        *from, *to});
    QApplication::processEvents();
    REQUIRE(scenario_model(*doc).cables.size() == 1);

    for(int format = 0; format < 2; format++)
    {
      CAPTURE(format);
      auto* loaded = format == 0 ? score::test::reload_via_json(ctx, *doc)
                                 : score::test::reload_via_bytes(ctx, *doc);
      REQUIRE(loaded);
      // As Granola (v1), its ports, values and cable as they were
      const auto found = processes_of(*loaded, granola_v1_uuid);
      REQUIRE(found.size() == 2);
      Process::ProcessModel* g{};
      for(auto* p : found)
        if(p->id() == granola->id())
          g = p;
      REQUIRE(g);
      CHECK(g->inlets().size() == granola->inlets().size());
      for(auto& [name, v] : values)
      {
        CAPTURE(name.toStdString());
        auto* inlet = qobject_cast<Process::ControlInlet*>(port_named(g->inlets(), name));
        REQUIRE(inlet);
        CHECK(ossia::convert<float>(inlet->value()) == v);
      }
      auto& cables = scenario_model(*loaded).cables;
      REQUIRE(cables.size() == 1);
      auto& sink = cables.begin()->sink().find(loaded->context());
      CHECK(sink.name() == "Pitch Jitter Range");
      CHECK(&sink == port_named(g->inlets(), "Pitch Jitter Range"));
    }
  });
}

TEST_CASE("The new Granola is another process, without the Jitter Range ports", "[app][v1]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto* g = add_by_uuid(*doc, QString::fromUtf8(G::uuid()));
    REQUIRE(g);
    CHECK(g->concreteKey() != UuidKey<Process::ProcessModel>::fromString(granola_v1_uuid));
    CHECK(port_named(g->inlets(), "Position Deviation"));
    CHECK(!port_named(g->inlets(), "Position Jitter Range"));
    CHECK(!port_named(g->inlets(), "Reserved"));
  });
}

TEST_CASE("A document saved by the Granola released first loads as Granola (v1)", "[app][v1]")
{
  // Saved by score with the first Granola's own code (origin/main at
  // a45445a): two of them, values on the Jitter Range ports, a cable from
  // one's Active Grains into the other's Pitch Jitter Range
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QFile file{QStringLiteral(GRANOLA_V1_DOCUMENT)};
    REQUIRE(file.open(QIODevice::ReadOnly));
    auto& delegates = ctx.interfaces<score::DocumentDelegateList>();
    REQUIRE(!delegates.empty());
    auto* doc = ctx.docManager.loadDocument(
        ctx, QStringLiteral("granola-v1"), file.readAll(), JSONObject::type(),
        *delegates.begin());
    spin();
    REQUIRE(doc);

    const auto found = processes_of(*doc, granola_v1_uuid);
    REQUIRE(found.size() == 2);
    Process::ProcessModel* g{};
    for(auto* p : found)
      if(p->id() == Id<Process::ProcessModel>{2})
        g = p;
    REQUIRE(g);
    QStringList names;
    for(auto* i : g->inlets())
      names.push_back(i->name());
    CHECK(
        names
        == QStringList{
            "Sound", "Position", "Position Jitter", "Position Jitter Range", "Duration",
            "Duration Jitter", "Duration Jitter Range", "Pitch", "Pitch Jitter",
            "Pitch Jitter Range", "Reverse", "Density", "Density Jitter",
            "Density Jitter Range", "Gain", "Gain Jitter", "Gain Jitter Range",
            "Window coefs", "Interpolation mode", "Window mode", "Loop", "Source Channels",
            "Channel Offset", "Max Voices", "Continuous", "Trigger", "MIDI In",
            "listening to MIDI"});
    const std::vector<std::pair<QString, float>> values{
        {"Position Jitter", 0.4f},       {"Position Jitter Range", 0.3f},
        {"Duration Jitter Range", 0.6f}, {"Density Jitter Range", 12.f},
        {"Gain Jitter Range", 0.25f},    {"Density", 7.f}, {"Pitch Jitter Range", 1.f}};
    for(auto& [name, v] : values)
    {
      CAPTURE(name.toStdString());
      auto* inlet = qobject_cast<Process::ControlInlet*>(port_named(g->inlets(), name));
      REQUIRE(inlet);
      CHECK(ossia::convert<float>(inlet->value()) == v);
    }
    auto& cables = scenario_model(*doc).cables;
    REQUIRE(cables.size() == 1);
    auto& sink = cables.begin()->sink().find(doc->context());
    CHECK(&sink == port_named(g->inlets(), "Pitch Jitter Range"));

    // And it plays: the executor of each, built and run
    auto& plug = load_execution(*doc);
    run_exec(plug);
    drain_gc(plug);
  });
}



TEST_CASE("The panel's Grain, Pitch and Output columns line up", "[app][ui]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    Mounted m{*doc, plug};
    spin(50);

    // Where each control's port sits: at the left of its item
    std::map<QString, double> x;
    for(auto* it : m.scene.items())
      if(auto* port = dynamic_cast<Dataflow::PortItem*>(it))
        if(m.panel->isAncestorOf(port))
          x[port->port().name()] = port->scenePos().x();
    const std::vector<std::vector<QString>> columns{
        {"Continuous", "Density", "Max Voices", "Low note", "On MIDI"},
        {"Trigger", "Density Deviation", "Velocity", "High note"},
        {"Position", "Duration", "Sample Start", "Loop"},
        {"Position Deviation", "Duration Deviation", "Sample End"},
        {"Scan", "Scan Mode", "Scan in folder"},
        {"Pitch", "Pitch Deviation in Semitones", "Interpolation mode"},
        {"Pitch Deviation", "Reverse Chance"},
        {"Gain", "Spread", "Source Channels"},
        {"Gain Deviation", "Output Channels", "Channel Offset"}};
    for(const auto& column : columns)
    {
      CAPTURE(column.front().toStdString());
      for(const auto& name : column)
      {
        CAPTURE(name.toStdString());
        REQUIRE(x.contains(name));
        CHECK(x[name] == Catch::Approx(x[column.front()]).margin(0.5));
      }
    }

    // Play first, then Grain, Pitch, Output, Window
    CHECK(x["Continuous"] < x["Position"]);
    CHECK(x["Position Deviation"] < x["Pitch"]);
    CHECK(x["Pitch Deviation"] < x["Gain"]);
    CHECK(x["Gain Deviation"] < x["Window mode"]);

    // GRANOLA_PANEL_PNG=path.png: the panel as drawn, to look at, with a
    // Sound file (path-file.png) and with a folder (path-folder.png)
    if(const auto path = qEnvironmentVariable("GRANOLA_PANEL_PNG"); !path.isEmpty())
    {
      const auto save = [&](const QString& suffix) {
        spin(100);
        const QRectF r = m.panel->sceneBoundingRect();
        QImage img((r.size() * 2).toSize(), QImage::Format_ARGB32);
        img.fill(Qt::black);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        m.scene.render(&p, QRectF{QPointF{}, r.size() * 2}, r);
        p.end();
        QString file = path;
        img.save(file.replace(".png", suffix + ".png"));
      };
      save("");
      QTemporaryDir dir;
      for(auto name : {"Back2.wav", "Kick.wav", "Snare long name.wav"})
        write_wav(dir.filePath(name), 220, 9600, 1);
      m.set("Sound", dir.filePath("Kick.wav").toStdString());
      m.play(20);
      save("-file");
      m.set("Sounds", dir.path().toStdString());
      m.set("Source", std::string("Folder"));
      m.set("Sound in folder", std::string("Back2.wav"));
      m.play_until([&] { return m.object().inputs.sounds.size() == 3; });
      m.play(20);
      save("-folder");
    }
  });
}

namespace
{
// The Source selector of the panel: Granola's own item, as large as a dropdown
QGraphicsItem* source_selector(Mounted& m)
{
  for(auto* it : m.scene.items())
    if(m.panel->isAncestorOf(it)
       && it->boundingRect()
              == QRectF{0., 0., Granola::SourceItem::width(), Granola::SourceItem::height()})
      return it;
  return nullptr;
}
void click(Mounted& m, QGraphicsItem* it, QPointF at)
{
  const QPoint p = m.view.mapFromScene(it->mapToScene(at));
  m.mouse(QEvent::MouseButtonPress, p, Qt::LeftButton);
  m.mouse(QEvent::MouseButtonRelease, p, Qt::NoButton);
  spin(20);
}
std::string text_of(Mounted& m, const QString& name)
{
  return ossia::convert<std::string>(m.inlet(name).value());
}
//! The Source port's value, a name or the index of its entry
bool source_is(Mounted& m, const std::string& name)
{
  const auto& v = m.inlet("Source").value();
  if(auto s = v.target<std::string>())
    return *s == name;
  return ossia::convert<int>(v) == (name == "Folder" ? 1 : 0);
}
}

TEST_CASE("Picking Folder in the panel takes the Sound file's folder, as one undo step", "[app][ui]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QTemporaryDir dir, other;
    write_wav(dir.filePath("a.wav"), 220, 4800, 1);
    write_wav(dir.filePath("b.wav"), 330, 4800, 1);
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    auto& stack = doc->commandStack();
    Mounted m{*doc, plug};
    m.set("Sound", dir.filePath("b.wav").toStdString());
    auto* selector = source_selector(m);
    REQUIRE(selector);

    // A click on the selector: Folder, the Sound file's folder and the file
    const auto picked = text_of(m, "Sound in folder");
    const int before = stack.currentIndex();
    click(m, selector, {20., 11.});
    CHECK(source_is(m, "Folder"));
    CHECK(
        QFileInfo{QString::fromStdString(text_of(m, "Sounds"))}.canonicalFilePath()
        == QFileInfo{dir.path()}.canonicalFilePath());
    CHECK(text_of(m, "Sound in folder") == "b.wav");
    CHECK(stack.currentIndex() == before + 1);
    // The folder's files play
    CHECK(m.play_until([&] { return m.object().inputs.sounds.size() == 2; }));

    // Undone at once, all three
    stack.undoQuiet();
    spin(20);
    CHECK(source_is(m, "File"));
    CHECK(text_of(m, "Sounds").empty());
    CHECK(text_of(m, "Sound in folder") == picked);

    // A Folder set already stays
    m.set("Sounds", other.path().toStdString());
    m.set("Sound in folder", std::string("x.wav"));
    click(m, selector, {20., 11.});
    CHECK(source_is(m, "Folder"));
    CHECK(text_of(m, "Sounds") == other.path().toStdString());
    CHECK(text_of(m, "Sound in folder") == "x.wav");

    // The strip: - back to File, + Folder; a click on the source shown does
    // nothing
    click(m, selector, {58., 17.});
    CHECK(source_is(m, "File"));
    const int at_file = stack.currentIndex();
    click(m, selector, {58., 17.});
    CHECK(stack.currentIndex() == at_file);
    click(m, selector, {58., 5.});
    CHECK(source_is(m, "Folder"));
  });
}

TEST_CASE("Source set to Folder as automation does changes nothing else", "[app][ui]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QTemporaryDir dir;
    write_wav(dir.filePath("a.wav"), 220, 4800, 1);
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    Mounted m{*doc, plug};
    m.set("Sound", dir.filePath("a.wav").toStdString());
    const auto picked = text_of(m, "Sound in folder");
    const int before = doc->commandStack().currentIndex();
    // Through the port, as automation, a cable, OSC or a preset
    m.inlet("Source").setValue(std::string("Folder"));
    m.play(10);
    spin(20);
    CHECK(source_is(m, "Folder"));
    CHECK(text_of(m, "Sounds").empty());
    CHECK(text_of(m, "Sound in folder") == picked);
    CHECK(doc->commandStack().currentIndex() == before);
    // The processor plays the Sound file meanwhile
    CHECK(m.play_until([&] { return m.playing_grains(); }));
  });
}

TEST_CASE("On MIDI's entries reach the processor as chosen", "[app][midi]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    Mounted m{*doc, plug};
    using on = std::decay_t<decltype(G{}.inputs.on_midi.value)>;
    for(auto [name, want] : std::vector<std::pair<std::string, on>>{
            {"Trigger", on::Trigger},
            {"Trigger and set pitch", on::TriggerPitch},
            {"Nothing", on::Nothing},
            {"Set pitch", on::SetPitch},
            {"Set position", on::SetPosition},
            {"Trigger and set position", on::TriggerPosition},
            {"Trigger", on::Trigger}})
    {
      CAPTURE(name);
      m.set("On MIDI", name);
      m.play(3);
      CHECK(m.object().inputs.on_midi.value == want);
    }
  });
}


TEST_CASE("Every dropdown's entries reach the processor as chosen", "[app][ui]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    Mounted m{*doc, plug};
    auto& in = m.object().inputs;
    // Each entry by its name, as the dropdown sends it, then its index in
    // the processor
    const auto check = [&](const QString& port, std::vector<std::string> names, auto get) {
      for(std::size_t k = 0; k < names.size(); k++)
      {
        CAPTURE(port.toStdString(), names[k]);
        m.set(port, names[k]);
        m.play(3);
        CHECK(int(get()) == int(k));
      }
    };
    check("Sound Order",
          {"Single file", "Random", "Shuffle", "Alphabetical", "Ping-pong", "Random walk",
           "By MIDI note"},
          [&] { return in.order.value; });
    check("Scan in folder", {"Restart", "Continue", "Per file"},
          [&] { return in.scan_folder.value; });
    check("Scan Mode", {"Straight", "Loop", "PingPong"}, [&] { return in.scan_mode.value; });
    check("Source", {"File", "Folder"}, [&] { return in.source.value; });
    check("Interpolation mode", {"None", "Linear", "Cubic"}, [&] { return in.interp_type.value; });
    check("Window mode", {"Beta", "Cos", "Kuma"}, [&] { return in.window_mode.value; });
    check("On MIDI",
          {"Nothing", "Trigger", "Trigger and set pitch", "Set pitch", "Set position",
           "Trigger and set position"},
          [&] { return in.on_midi.value; });
    // The note range, as its spin boxes send it
    m.set("Low note", 36);
    m.set("High note", 39);
    m.play(3);
    CHECK(in.low_note.value == 36);
    CHECK(in.high_note.value == 39);
  });
}

TEST_CASE("Values through the ports, as automation sends them, write nothing in the document", "[app][ui]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QTemporaryDir dir;
    write_wav(dir.filePath("a.wav"), 220, 4800, 1);
    write_wav(dir.filePath("b.wav"), 330, 4800, 1);
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    Mounted m{*doc, plug};
    m.set("Sound", dir.filePath("a.wav").toStdString());
    m.set("Sounds", dir.path().toStdString());
    const int before = doc->commandStack().currentIndex();
    // Through the ports while it plays, one value after another
    for(auto [port, v] : std::vector<std::pair<QString, ossia::value>>{
            {"Source", std::string("Folder")}, {"Sound Order", std::string("Random")},
            {"Sound in folder", std::string("1")}, {"On MIDI", std::string("Set position")},
            {"Source", std::string("File")}, {"Source", std::string("Folder")},
            {"Low note", 40}, {"Scan in folder", std::string("Per file")}})
    {
      CAPTURE(port.toStdString());
      m.inlet(port).setValue(v);
      m.play(5);
      spin(10);
    }
    CHECK(doc->commandStack().currentIndex() == before);
    CHECK(text_of(m, "Sounds") == dir.path().toStdString());
    CHECK(m.object().inputs.low_note.value == 40);
    // The last values reached the processor
    CHECK(m.play_until([&] { return m.object().inputs.sounds.size() == 2; }));
    // An index names the file at that place of the dropdown's list, as score
    // maps numbers sent to a folder's dropdown
    CHECK(m.object().inputs.sound_file.value == "b.wav");
  });
}

TEST_CASE("Output Channels 1 with a stereo sound: one channel out, at any Spread", "[app][output]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QTemporaryDir dir;
    write_wav(dir.filePath("st.wav"), 220, 48000, 2);
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    Mounted m{*doc, plug};
    m.set("Sound", dir.filePath("st.wav").toStdString());
    REQUIRE(m.play_until([&] { return m.playing_grains(); }));

    auto outlet_channels = [&] {
      for(auto* o : m.comp->node->root_outputs())
        if(auto* a = o->target<ossia::audio_port>())
          return (int)a->channels();
      return -1;
    };
    struct expect
    {
      int src, outs;
      float spread;
      int channels;
    };
    for(auto c : std::vector<expect>{
            {0, 1, 0.f, 1},
            {0, 1, 1.f, 1},
            {0, 1, 2.f, 1},
            {1, 1, 2.f, 1},
            {1, 0, 0.f, 1},
            // Output Channels 0: as many as read, but Spread needs two
            {1, 0, 1.f, 2},
            {0, 0, 1.f, 2},
            {0, 2, 0.f, 2}})
    {
      CAPTURE(c.src, c.outs, c.spread);
      m.set("Source Channels", c.src);
      m.set("Output Channels", c.outs);
      m.set("Spread", c.spread);
      m.play(20);
      CAPTURE((int)m.object().outputs.audio.channels);
      CHECK(outlet_channels() == c.channels);
    }
  });
}

namespace
{
using WaveformControl = std::remove_reference_t<decltype(std::declval<G::ui&>().sound.waveform)>;
//! What the panel's waveform shows: the item it draws from
struct PeekWaveform : oscr::CustomMultiControl<WaveformControl&>
{
  using oscr::CustomMultiControl<WaveformControl&>::impl;
};
}

TEST_CASE("Duration sent to the port while it plays: the waveform shows it apart", "[app][ui]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QTemporaryDir dir;
    write_wav(dir.filePath("a.wav"), 220, 48000, 1);
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    Mounted m{*doc, plug};
    m.set("Sound", dir.filePath("a.wav").toStdString());
    m.set("Duration", 0.2f);
    m.play(10);

    auto& dur_inlet = m.inlet("Duration");
    int model_changes = 0, exec_changes = 0;
    QObject::connect(&dur_inlet, &Process::ControlInlet::valueChanged, [&](auto&) { model_changes++; });
    QObject::connect(&dur_inlet, &Process::ControlInlet::executionValueChanged, [&](auto&) { exec_changes++; });

    auto* wf = static_cast<PeekWaveform*>(
        dynamic_cast<oscr::CustomMultiControl<WaveformControl&>*>(m.waveform()));
    REQUIRE(wf);
    auto& item = wf->impl;
    REQUIRE(!item.execution_window());

    // The node's inlet of Duration, as a cable from another process fills it
    const auto& inl = m.model->inlets();
    const auto idx = std::distance(inl.begin(), std::ranges::find(inl, &dur_inlet));
    auto* port = m.comp->node->root_inputs()[idx]->target<ossia::value_port>();
    REQUIRE(port);
    for(int k = 0; k < 20; k++)
    {
      port->write_value(0.6f, 0);
      m.tick();
      spin(5);
    }
    CAPTURE(model_changes, exec_changes);
    CHECK(m.object().inputs.dur.value == Catch::Approx(0.6f));
    REQUIRE(exec_changes > 0);

    // The value the user set, in the model and in the item, is the same
    CHECK(model_changes == 0);
    CHECK(ossia::convert<float>(dur_inlet.value()) == Catch::Approx(0.2f));
    CHECK(item.dur() == Catch::Approx(0.2f));
    // What execution plays, apart
    using WF = Granola::WaveformItem;
    REQUIRE(item.execution_values[WF::DUR]);
    CHECK(*item.execution_values[WF::DUR] == Catch::Approx(0.6));
    const auto ew = item.execution_window();
    REQUIRE(ew);
    CHECK(ew->second - ew->first == Catch::Approx(0.6 * item.draw_w()).margin(1.));
    CHECK(item.win_x1() - item.win_x0() == Catch::Approx(0.2 * item.draw_w()).margin(1.));

    // Stopped: the window execution played is no longer shown. The values
    // of the last ticks reach the panel first.
    spin(100);
    m.model->resetExecution();
    spin(5);
    CHECK(!item.execution_values[WF::DUR]);
    CHECK(!item.execution_window());
    CHECK(item.dur() == Catch::Approx(0.2f));
  });
}

TEST_CASE("A vertical drag of the window scales it, at any length of the sound", "[ui]")
{
  using WF = Granola::WaveformItem;
  auto drag = [](std::array<double, 6> values, double x_of_window, double dy, int index) {
    WF w;
    w.set_width(600.);
    w.waveform.ready = true;
    w.values = values;
    std::optional<double> got;
    w.transaction.update = [&](int i, double v) {
      if(i == index)
        got = v;
    };
    const double x0 = w.win_x0(), x1 = w.win_x1();
    const double x = x_of_window < 0. ? x0 : x_of_window > 1. ? x1 : x0 + x_of_window * (x1 - x0);
    REQUIRE(w.mouse_press(x, 80.));
    w.mouse_move(x, 80. - dy);
    REQUIRE(got);
    return *got;
  };

  SECTION("Duration: twice as long every 40 pixels up, half as long down")
  {
    for(double d : {0.2, 0.01, 0.002})
    {
      CAPTURE(d);
      const std::array<double, 6> v{0.2, d, 0., 0., 0., 1.};
      CHECK(drag(v, 0.5, 40., WF::DUR) == Catch::Approx(2. * d));
      CHECK(drag(v, 0.5, 80., WF::DUR) == Catch::Approx(4. * d));
      CHECK(drag(v, 0.5, -40., WF::DUR) == Catch::Approx(d / 2.));
    }
    // At most the region
    CHECK(drag({0.2, 0.6, 0., 0., 0., 1.}, 0.5, 80., WF::DUR) == Catch::Approx(1.));
    // Centred: Position moves back by half of what Duration grew
    CHECK(drag({0.2, 0.1, 0., 0., 0., 1.}, 0.5, 40., WF::POS) == Catch::Approx(0.15));
  }
  SECTION("The Deviations: Deviation and Duration together scale")
  {
    // From 0 up: a Duration more for 40 pixels; down to 0 again
    const std::array<double, 6> v{0.2, 0.2, 0., 0.6, 0., 1.};
    CHECK(drag(v, -1., 40., WF::POS_J) == Catch::Approx(0.2));
    CHECK(drag(v, -1., -40., WF::POS_J) == Catch::Approx(0.));
    CHECK(drag(v, 2., -40., WF::DUR_J) == Catch::Approx(0.2));
    CHECK(drag(v, 2., 40., WF::DUR_J) == Catch::Approx(1.));
    // A small Deviation: steps in the scale of the grain, not of the region
    CHECK(drag({0.2, 0.1, 0.001, 0., 0., 1.}, -1., 40., WF::POS_J) == Catch::Approx(0.102));
  }
  SECTION("From a Duration of ~0, a pixel of the region is what doubles")
  {
    // 600 pixels wide: the region is drawn on 596
    const double pixel = 1. / 596.;
    for(double d : {0., 1e-8, 0.0001})
    {
      CAPTURE(d);
      const std::array<double, 6> v{0.2, d, 0., 0., 0., 1.};
      CHECK(drag(v, 0.5, 40., WF::DUR) == Catch::Approx(d + pixel));
      CHECK(drag(v, 0.5, 80., WF::DUR) == Catch::Approx(d + 3. * pixel));
      CHECK(drag(v, 0.5, 400., WF::DUR) == Catch::Approx(1.));
      CHECK(drag(v, 0.5, -40., WF::DUR) == Catch::Approx(1e-8).margin(1e-6));
    }
    // A sound half as long on screen: the pixel is twice the part of it
    const std::array<double, 6> half{0.2, 0., 0., 0., 0., 0.5};
    CHECK(drag(half, 0.5, 40., WF::DUR) == Catch::Approx(2. * pixel));
  }
  SECTION("A drag of any size gives a number")
  {
    for(double d : {0., 0.1})
      for(double dy : {1e4, 1e9, -1e4, -1e9})
      {
        CAPTURE(d, dy);
        const std::array<double, 6> v{0.2, d, 0., 0., 0., 1.};
        for(int index : {WF::DUR, WF::POS})
        {
          const double got = drag(v, 0.5, dy, index);
          CHECK(std::isfinite(got));
        }
        CHECK(drag(v, 0.5, dy, WF::DUR) == Catch::Approx(dy > 0 ? 1. : 1e-8).margin(1e-6));
        // The ends of a window wide enough
        if(d > 0.)
        {
          CHECK(std::isfinite(drag(v, -1., dy, WF::POS_J)));
          CHECK(std::isfinite(drag(v, 2., dy, WF::DUR_J)));
        }
      }
  }
}

// ---------------------------------------------------------------- Duration Mode

namespace
{
//! The node's inlet of a control, as a cable from another process fills it
ossia::value_port* node_port(Mounted& m, const QString& name)
{
  auto& inlet = m.inlet(name);
  const auto& inl = m.model->inlets();
  const auto idx = std::distance(inl.begin(), std::ranges::find(inl, &inlet));
  return m.comp->node->root_inputs()[idx]->target<ossia::value_port>();
}
//! A sound file of \p seconds at 48 kHz, in \p dir, set as the Sound
void use_sound(Mounted& m, QTemporaryDir& dir, double seconds)
{
  const auto path = dir.filePath("s.wav");
  write_wav(path, 220, int(seconds * 48000), 1);
  m.set("Sound", path.toStdString());
  REQUIRE(m.play_until([&] { return m.object().inputs.sound.frames() > 0; }));
}
//! Each value of \p v near \p want
void all_near(const std::vector<int64_t>& v, double want, double margin)
{
  REQUIRE(!v.empty());
  for(auto x : v)
    CHECK(double(x) == Catch::Approx(want).margin(margin));
}
//! Each grain of \p g lasted \p samples
void all_lasted(const std::vector<Mounted::GrainLog::grain>& g, double samples)
{
  REQUIRE(!g.empty());
  for(auto& x : g)
  {
    CAPTURE(x.start, x.played, x.within, samples);
    CHECK(x.lasted(samples));
  }
}
std::vector<int64_t> intervals(const std::vector<int64_t>& starts)
{
  std::vector<int64_t> res;
  for(std::size_t k = 1; k < starts.size(); k++)
    res.push_back(starts[k] - starts[k - 1]);
  return res;
}
}

TEST_CASE("Duration Time synced: an eighth is 0.25 s at 120 BPM and 0.5 s at 60, from the next grain on", "[app][duration-mode]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QTemporaryDir dir;
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    Mounted m{*doc, plug};
    use_sound(m, dir, 4.);
    m.set("Duration Mode", std::string("Time"));
    // An eighth note, restarting at the bar lines, as the knob gives it
    m.set("Duration Time", ossia::vec2f{1.f / 8.f, 2.f});
    m.set("Keep time", true);
    m.set("Density", 1.f);
    m.play(10);
    CHECK(m.object().inputs.dur_time.sync);
    CHECK(m.object().inputs.dur_time.value == Catch::Approx(0.25f));
    // Samples at the engine's rate
    const double sr = m.object().samplerate;

    // 120 BPM: grains of 0.25 s, 0.25 s apart
    const int64_t from = m.at;
    while(m.at < from + 48000 * 2)
      m.tick();
    all_lasted(m.log.over(from), 0.25 * sr);
    all_near(intervals(m.log.starts(from)), 0.25 * sr, 1.);

    // 60 BPM: the grain playing then keeps its length
    const auto playing = m.log.starts().back();
    const int64_t changed = m.at;
    m.tempo = 60.;
    while(m.at < changed + 48000 * 3)
      m.tick();
    CHECK(m.object().inputs.dur_time.value == Catch::Approx(0.5f));
    bool seen = false;
    for(auto& g : m.log.grains)
      if(g.start == playing)
      {
        seen = true;
        CHECK(g.lasted(0.25 * sr));
      }
    CHECK(seen);
    // Those after: 0.5 s, and 0.5 s apart from the one playing on: the pace
    // of the tick after the change
    auto after = m.log.starts(changed);
    REQUIRE(after.size() >= 3);
    all_lasted(m.log.over(changed), 0.5 * sr);
    after.insert(after.begin(), playing);
    all_near(intervals(after), 0.5 * sr, 1.);
  });
}

TEST_CASE("Duration Time free: seconds, at any tempo; a plain number sent to it is seconds", "[app][duration-mode]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QTemporaryDir dir;
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    Mounted m{*doc, plug};
    use_sound(m, dir, 4.);
    m.set("Duration Mode", std::string("Time"));
    m.set("Duration Time", ossia::vec2f{0.3f, 0.f});
    m.set("Density", 0.5f);
    for(double tempo : {120., 60.})
    {
      CAPTURE(tempo);
      m.tempo = tempo;
      m.play(5);
      const int64_t from = m.at;
      while(m.at < from + 48000 * 3)
        m.tick();
      CHECK(!m.object().inputs.dur_time.sync);
      all_lasted(m.log.over(from), 0.3 * m.object().samplerate);
    }

    // A number into the control, as the inspector or a preset sends it:
    // seconds
    m.set("Duration Time", 0.2f);
    CHECK(m.inlet("Duration Time").value() == ossia::value{ossia::vec2f{0.2f, 0.f}});
    // And through the node's inlet, as a cable or OSC sends it
    auto* port = node_port(m, "Duration Time");
    REQUIRE(port);
    const int64_t from = m.at;
    while(m.at < from + 48000 * 3)
    {
      port->write_value(0.1f, 0);
      m.tick();
    }
    CHECK(!m.object().inputs.dur_time.sync);
    CHECK(m.object().inputs.dur_time.value == Catch::Approx(0.1f));
    all_lasted(m.log.over(from + 48000), 0.1 * m.object().samplerate);
  });
}

TEST_CASE("A document saved before Duration had modes plays as it did", "[app][duration-mode]")
{
  // Saved by c3f2b2e: Duration 0.2, Duration Deviation 0, Keep time on,
  // Density 2, an automation into Duration
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QFile file{QStringLiteral(GRANOLA_BEFORE_MODES_DOCUMENT)};
    REQUIRE(file.open(QIODevice::ReadOnly));
    const auto saved = file.readAll();
    auto& delegates = ctx.interfaces<score::DocumentDelegateList>();
    REQUIRE(!delegates.empty());
    auto* loaded = ctx.docManager.loadDocument(
        ctx, QStringLiteral("granola-before-modes"), saved, JSONObject::type(),
        *delegates.begin());
    spin();
    REQUIRE(loaded);
    for(int format = 0; format < 3; format++)
    {
      CAPTURE(format);
      score::Document* doc = format == 0   ? loaded
                             : format == 1 ? score::test::reload_via_json(ctx, *loaded)
                                           : score::test::reload_via_bytes(ctx, *loaded);
      REQUIRE(doc);
      const auto found = processes_of(*doc, QString::fromUtf8(G::uuid()));
      REQUIRE(found.size() == 1);
      auto* g = dynamic_cast<oscr::ProcessModel<G>*>(found.front());
      REQUIRE(g);
      auto value = [&](const QString& name) {
        auto* inlet = qobject_cast<Process::ControlInlet*>(port_named(g->inlets(), name));
        REQUIRE(inlet);
        return inlet->value();
      };
      CHECK(ossia::convert<float>(value("Duration")) == Catch::Approx(0.2f));
      CHECK(ossia::convert<float>(value("Duration Deviation")) == 0.f);
      CHECK(ossia::convert<bool>(value("Keep time")));
      CHECK(ossia::convert<float>(value("Density")) == Catch::Approx(2.f));
      // The new controls at their inits: Region, which is how it played
      const auto mode = value("Duration Mode");
      CHECK((mode == ossia::value{std::string("Region")} || mode == ossia::value{0}));
      CHECK(value("Duration Time") == ossia::value{ossia::vec2f{0.1f, 0.f}});
      // The automation's cable still goes into Duration
      auto& cables = scenario_model(*doc).cables;
      REQUIRE(cables.size() == 1);
      auto& sink = cables.begin()->sink().find(doc->context());
      CHECK(&sink == port_named(g->inlets(), "Duration"));

      // Played: grains of Duration's part of the region, the automation's
      // values as parts of it too
      auto& plug = load_execution(*doc);
      {
        QTemporaryDir dir;
        Mounted m{*doc, plug, g};
        use_sound(m, dir, 2.);
        m.play(20);
        CHECK(m.object().inputs.dur_mode.value
              == decltype(m.object().inputs.dur_mode.value)::Region);
        int64_t from = m.at;
        while(m.at < from + 48000 * 3)
          m.tick();
        // Keep time at Pitch 1: 0.2 of the sound
        const double frames = m.object().inputs.sound.frames();
        all_lasted(m.log.over(from), 0.2 * frames);
        auto* port = node_port(m, "Duration");
        REQUIRE(port);
        from = m.at;
        while(m.at < from + 48000 * 4)
        {
          port->write_value(0.35f, 0);
          m.tick();
        }
        all_lasted(m.log.over(from + 48000), 0.35 * frames);
      }
    }
  });
}

TEST_CASE("A preset saved before Duration had modes loads as it was saved", "[app][duration-mode]")
{
  score::test::run_in_app([](const score::GUIApplicationContext& ctx) {
    QFile file{QStringLiteral(GRANOLA_BEFORE_MODES_PRESET)};
    REQUIRE(file.open(QIODevice::ReadOnly));
    Process::Preset preset;
    preset.key.key = UuidKey<Process::ProcessModel>::fromString(QString::fromUtf8(G::uuid()));
    preset.data = file.readAll();
    auto* doc = score::test::new_document(ctx);
    REQUIRE(doc);
    auto& plug = load_execution(*doc);
    Mounted m{*doc, plug};
    m.set("Duration", 0.7f);
    m.set("Keep time", false);
    m.model->loadPreset(preset);
    m.play(5);
    auto& in = m.object().inputs;
    CHECK(in.dur.value == Catch::Approx(0.2f));
    CHECK(in.keep_time.value);
    CHECK(in.density.value == Catch::Approx(2.f));
    CHECK(in.dur_mode.value == decltype(in.dur_mode.value)::Region);
    CHECK(in.dur_time.value == Catch::Approx(0.1f));
    // An instance in Time mode stays so: a preset sets the controls it has
    m.set("Duration Mode", std::string("Time"));
    m.model->loadPreset(preset);
    m.play(5);
    CHECK(in.dur_mode.value == decltype(in.dur_mode.value)::Time);
    CHECK(in.dur.value == Catch::Approx(0.2f));
  });
}
