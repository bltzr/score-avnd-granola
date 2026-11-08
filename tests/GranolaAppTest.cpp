// Granola in score, its panel used as a user would while it plays: sounds and
// folders dropped, the waveform dragged, controls changed, undone and redone,
// the panel rebuilt and deleted, the process torn down mid-playback. The
// executor runs as the audio graph runs it: real tokens, the execution queue
// drained as the audio thread. Nothing may crash; the outputs stay finite.
#include "Granola/Granola.hpp"

#include <Process/Commands/SetControlValue.hpp>
#include <Process/Dataflow/Port.hpp>
#include <Process/DocumentPlugin.hpp>
#include <Process/Focus/FocusDispatcher.hpp>
#include <Process/LayerPresenter.hpp>
#include <Process/ProcessContext.hpp>

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
#include <thread>

namespace
{
using G = Granola::Granola;

ossia::token_request make_token(int64_t& at, int rate, int buffer)
{
  const int64_t per_sample = 705600000 / rate;
  ossia::token_request tk;
  tk.prev_date = ossia::time_value{per_sample * at};
  at += buffer;
  tk.date = ossia::time_value{per_sample * at};
  tk.parent_duration = ossia::time_value{per_sample * 48000 * 600};
  tk.speed = 1.;
  tk.tempo = 120.;
  tk.signature = ossia::time_signature{4, 4};
  tk.start_sample = 0;
  tk.length_sample = buffer;
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
  std::shared_ptr<oscr::Executor<G>> comp;
  int64_t at{};

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

  Mounted(score::Document& d, Execution::DocumentPlugin& p)
      : doc{d}
      , plug{p}
      , pctx{d.context(), dfm, fd}
  {
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
    n.run(make_token(at, 48000, buffer), ossia::exec_state_facade{plug.context().execState.get()});
    for(auto* i : n.root_inputs())
      if(auto* v = i->target<ossia::value_port>())
        v->get_data().clear();
    ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
    for(auto* o : n.root_outputs())
      if(auto* a = o->target<ossia::audio_port>())
        for(auto& chan : *a)
          for(double x : chan)
            finite &= std::isfinite(x);
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

    m.set("Random", true);
    m.set("Scan", 1.f);
    m.set("Scan Across Sounds", true);
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
