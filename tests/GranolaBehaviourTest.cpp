// What a host hears from Granola, a feature at a time: each test plays the
// processor as score does and measures what comes out (which sound and
// channel each grain read, where, how fast, how loud, when) rather than
// reading the processor's own state.
#include "GranolaBehaviourTest.hpp"

#include <avnd/introspection/input.hpp>
#include <avnd/introspection/output.hpp>
#include <avnd/wrappers/metadatas.hpp>

#include <set>

using namespace granola_host;

namespace
{
//! The grains heard over \p seconds, once those playing now are over
std::vector<Segment> heard(Host& h, double seconds, double settle = 0.1)
{
  h.run(settle);
  h.forget();
  h.run(seconds);
  return h.segments();
}
std::vector<int> files_of(const std::vector<Segment>& segs)
{
  std::vector<int> res;
  for(auto& s : segs)
    res.push_back(s.tag >= sound_tag ? -1 : s.tag - 1);
  return res;
}
std::set<int> file_set(const std::vector<Segment>& segs)
{
  const auto f = files_of(segs);
  return {f.begin(), f.end()};
}
std::vector<double> slopes(const std::vector<Segment>& segs)
{
  std::vector<double> res;
  for(auto& s : segs)
    if(s.length >= 8)
      res.push_back(s.slope);
  return res;
}
bool all_near(const std::vector<double>& v, double want, double tol)
{
  return !v.empty() && std::ranges::all_of(v, [=](double x) { return std::abs(x - want) <= tol; });
}
std::vector<double> rels(const std::vector<Segment>& segs)
{
  std::vector<double> res;
  for(auto& s : segs)
    res.push_back(s.rel);
  return res;
}
//! A note pressed and held for \p hold seconds, its grains heard
std::vector<Segment> note_heard(Host& h, int note, double hold = 0.1, int velocity = 127)
{
  h.forget();
  h.press(note, velocity);
  h.run(hold);
  h.release(note);
  h.run(0.1);
  return h.segments();
}
}

// ---------------------------------------------------------------- Sources

TEST_CASE("Source: File plays the Sound file, Folder the folder's, the Sound file while no folder is set", "[source]")
{
  Host h;
  Folder f{3};
  f.list_into(*h.g);
  h.g->inputs.sound_file.value = "1";
  auto segs = heard(h, 0.5);
  REQUIRE(segs.size() >= 4);
  CHECK(file_set(segs) == std::set{-1});

  h.g->inputs.source.value = source_kind::Folder;
  segs = heard(h, 0.5);
  REQUIRE(segs.size() >= 4);
  CHECK(file_set(segs) == std::set{1});

  // No folder set: the host lists no file, and the Sound file plays
  h.g->inputs.sounds.value.clear();
  h.g->inputs.sounds.files = {};
  segs = heard(h, 0.5);
  REQUIRE(segs.size() >= 4);
  CHECK(file_set(segs) == std::set{-1});
}

TEST_CASE("Sound in folder: by name, by index, the last for an index past it, the first for an unknown name", "[source]")
{
  for(auto [value, want] : std::vector<std::pair<std::string, int>>{
          {"f2.wav", 2}, {"1", 1}, {"9", 3}, {"2.7", 2}, {"nope.wav", 0}, {"f3.wav", 3}})
  {
    CAPTURE(value);
    Host h;
    Folder f{4};
    h.use(f, order::Single, value);
    const auto segs = heard(h, 0.3, 0.);
    REQUIRE(segs.size() >= 10);
    CHECK(file_set(segs) == std::set{want});
  }
}

TEST_CASE("Folder listed again: the file picked by name plays on, the index names the new listing", "[source][folder]")
{
  Host h;
  Folder f{3};
  h.use(f, order::Single, "f2.wav");
  CHECK(file_set(heard(h, 0.2)) == std::set{2});
  // A file before it: it is the fourth now, and still plays
  f.add("a.wav", 9, rate / 10, 0);
  f.list_into(*h.g);
  CHECK(file_set(heard(h, 0.2)) == std::set{2});
  // By index: the second of the listing, f0 now
  h.g->inputs.sound_file.value = "1";
  CHECK(file_set(heard(h, 0.2)) == std::set{0});
  // Alphabetical goes through the new file too
  h.g->inputs.order.value = order::Alphabetical;
  CHECK(file_set(heard(h, 0.3)) == std::set{8, 0, 1, 2});
}

// ---------------------------------------------------------------- Sound Order

TEST_CASE("Sound Order Single file: every grain reads the picked file", "[order]")
{
  Host h;
  Folder f{5};
  h.use(f, order::Single, "3");
  const auto segs = heard(h, 0.5, 0.);
  REQUIRE(segs.size() >= 40);
  CHECK(file_set(segs) == std::set{3});
}

TEST_CASE("Sound Order Random: every file as often, independently", "[order]")
{
  Host h;
  Folder f{4};
  h.use(f, order::Random);
  const auto seq = files_of(heard(h, 20., 0.));
  REQUIRE(seq.size() > 1800);
  std::map<int, int> count;
  int same = 0;
  for(std::size_t k = 0; k < seq.size(); k++)
  {
    count[seq[k]]++;
    same += k > 0 && seq[k] == seq[k - 1];
  }
  for(int k = 0; k < 4; k++)
  {
    CAPTURE(k);
    CHECK(count[k] == Catch::Approx(seq.size() / 4.).epsilon(0.15));
  }
  // A file again as often as any other: a quarter of the time
  CHECK(double(same) / seq.size() == Catch::Approx(0.25).margin(0.05));
}

TEST_CASE("Sound Order Shuffle: every file once a cycle, never the same twice in a row", "[order]")
{
  for(int n : {2, 3, 5})
  {
    CAPTURE(n);
    Host h;
    Folder f{n};
    h.use(f, order::Shuffle);
    // From the first grain: the first cycle starts with it
    h.forget();
    h.run(0.0001);
    const auto seq = [&] {
      h.run(n * 0.8);
      return files_of(h.segments());
    }();
    const int cycles = int(seq.size()) / n;
    REQUIRE(cycles >= 30);
    std::set<std::vector<int>> orders;
    for(int c = 0; c < cycles; c++)
    {
      std::vector<int> cycle(seq.begin() + c * n, seq.begin() + (c + 1) * n);
      orders.insert(cycle);
      std::ranges::sort(cycle);
      std::vector<int> all(n);
      std::iota(all.begin(), all.end(), 0);
      CAPTURE(c);
      CHECK(cycle == all);
    }
    for(std::size_t k = 1; k < seq.size(); k++)
    {
      CAPTURE(k);
      CHECK(seq[k] != seq[k - 1]);
    }
    // Another order each cycle, not one again and again (two files only
    // have one that does not repeat)
    if(n > 2)
      CHECK(orders.size() > 1);
  }
}

TEST_CASE("Sound Order Alphabetical: the folder's order, around again", "[order]")
{
  Host h;
  Folder f{4};
  h.use(f, order::Alphabetical);
  h.forget();
  h.run(0.5);
  const auto seq = files_of(h.segments());
  REQUIRE(seq.size() >= 40);
  const int first = seq[0];
  for(std::size_t k = 0; k < seq.size(); k++)
  {
    CAPTURE(k);
    CHECK(seq[k] == int(first + k) % 4);
  }
}

TEST_CASE("Sound Order Ping-pong: to the last and back, the ends once", "[order]")
{
  Host h;
  Folder f{4};
  h.use(f, order::PingPong);
  h.forget();
  h.run(0.0001);
  h.run(0.5);
  const auto seq = files_of(h.segments());
  REQUIRE(seq.size() >= 30);
  const std::vector<int> period{0, 1, 2, 3, 2, 1};
  // The first grain may be lost to the cut: where in the period it starts
  const auto at = [&](int p) {
    for(int k = 0; k < 12; k++)
      if(seq[k] != period[(p + k) % 6])
        return false;
    return true;
  };
  int phase = -1;
  for(int p = 0; p < 6 && phase < 0; p++)
    if(at(p))
      phase = p;
  REQUIRE(phase >= 0);
  for(std::size_t k = 0; k < seq.size(); k++)
  {
    CAPTURE(k);
    CHECK(seq[k] == period[(phase + k) % 6]);
  }
}

TEST_CASE("Sound Order Random walk: a step or two either way, back from the ends, never the same file", "[order]")
{
  Host h;
  Folder f{6};
  h.use(f, order::RandomWalk, "3");
  h.forget();
  h.run(10.);
  const auto seq = files_of(h.segments());
  REQUIRE(seq.size() > 900);
  int ones = 0;
  std::set<int> seen;
  for(std::size_t k = 1; k < seq.size(); k++)
  {
    const int d = std::abs(seq[k] - seq[k - 1]);
    CAPTURE(k, seq[k - 1], seq[k]);
    CHECK((d == 1 || d == 2));
    ones += d == 1;
    seen.insert(seq[k]);
  }
  CHECK(seen == std::set{0, 1, 2, 3, 4, 5});
  CHECK(double(ones) / seq.size() == Catch::Approx(0.8).margin(0.1));
}

TEST_CASE("Sound Order By MIDI note: Low note the first file, High note the last, the keys between spread over them", "[order][midi][range]")
{
  struct expect
  {
    int low, high, files;
    std::vector<std::pair<int, int>> notes; // note, file
  };
  const std::vector<expect> cases{
      // 25 keys, 4 files: the ends, between, and clamped past them
      {48, 72, 4, {{48, 0}, {72, 3}, {56, 1}, {64, 2}, {50, 0}, {70, 3}, {30, 0}, {100, 3}}},
      // More files than keys: the ends still reach the first and last
      {48, 72, 19, {{48, 0}, {72, 18}, {60, 9}, {49, 1}}},
      // Swapped: as the other way round
      {72, 48, 4, {{48, 0}, {72, 3}, {56, 1}, {64, 2}}},
      // One key: the first file
      {60, 60, 4, {{60, 0}, {72, 0}, {40, 0}}},
      // A small keyboard of 4 keys for 4 files
      {36, 39, 4, {{36, 0}, {37, 1}, {38, 2}, {39, 3}}}};
  for(const auto& c : cases)
  {
    CAPTURE(c.low, c.high, c.files);
    Host h;
    Folder f{c.files};
    h.use(f, order::ByNote);
    h.g->inputs.low_note.value = c.low;
    h.g->inputs.high_note.value = c.high;
    h.g->inputs.on_midi.value = on_midi::Trigger;
    h.continuous(false);
    for(auto [note, file] : c.notes)
    {
      CAPTURE(note);
      const auto segs = note_heard(h, note);
      REQUIRE(!segs.empty());
      CHECK(file_set(segs) == std::set{file});
    }
  }
}

TEST_CASE("Sound Order: files not ready or failed are never read, the others play", "[order]")
{
  for(auto o : {order::Random, order::Shuffle, order::Alphabetical, order::PingPong, order::RandomWalk})
  {
    CAPTURE(int(o));
    Host h;
    Folder f{4};
    // Their data still there, as a host may keep it: not to be read
    f.status(1, halp::sound_status::loading);
    f.status(2, halp::sound_status::failed);
    h.use(f, o, "0");
    const auto segs = heard(h, 1., 0.);
    REQUIRE(segs.size() > 50);
    CHECK(file_set(segs) == std::set{0, 3});
  }
  // By MIDI note: the note's file not ready, the next one that is
  Host h;
  Folder f{4};
  f.status(1, halp::sound_status::loading);
  f.status(2, halp::sound_status::failed);
  h.use(f, order::ByNote);
  h.g->inputs.on_midi.value = on_midi::Trigger;
  h.continuous(false);
  CHECK(file_set(note_heard(h, 56)) == std::set{3});
  // Single file of a file not ready: nothing
  Host s;
  Folder g{4};
  g.status(1, halp::sound_status::loading);
  s.use(g, order::Single, "1");
  s.run(0.3);
  CHECK(s.silent());
}

// ---------------------------------------------------------------- Scan

TEST_CASE("Scan: Position moves through the region at Scan times the sound's speed", "[scan]")
{
  for(auto [scan, lo, hi] : std::vector<std::tuple<float, float, float>>{
          {1.f, 0.f, 1.f}, {0.5f, 0.f, 1.f}, {-0.5f, 0.f, 1.f}, {1.f, 0.5f, 1.f}, {2.f, 0.25f, 0.75f}})
  {
    CAPTURE(scan, lo, hi);
    Host h;
    auto& in = h.g->inputs;
    in.dur.value = 0.02f;
    in.scan.value = scan;
    in.sample_start.value = lo;
    in.sample_end.value = hi;
    h.run(0.0001);
    h.forget();
    h.run(0.6);
    const auto segs = h.segments();
    REQUIRE(segs.size() > 10);
    // Between consecutive grains, but where Scan goes around
    std::vector<double> speed;
    for(std::size_t k = 1; k < segs.size(); k++)
    {
      const double v = (segs[k].first - segs[k - 1].first) / double(segs[k].start - segs[k - 1].start);
      if(std::abs(v) < 3.)
        speed.push_back(v);
    }
    REQUIRE(speed.size() > segs.size() / 2);
    std::ranges::sort(speed);
    CHECK(speed[speed.size() / 2] == Catch::Approx(scan).margin(0.01));
    // Never outside of the region
    for(auto& s : segs)
    {
      CHECK(s.rel >= lo - 1e-3);
      CHECK(s.rel <= hi + 1e-3);
    }
  }
}

TEST_CASE("Scan Mode: around, held, or back at the region's end or where a whole grain fits", "[scan]")
{
  // Position 0.1, Duration 0.05, Scan 1 for 1.25 s: past the end
  const std::tuple<scan_mode, bool, double> expect[] = {
      {scan_mode::Loop, false, 0.40},    {scan_mode::PingPong, false, 0.55},
      {scan_mode::Straight, false, 0.95}, {scan_mode::Loop, true, 0.35},
      {scan_mode::PingPong, true, 0.65},  {scan_mode::Straight, true, 1.}};
  for(auto [mode, wrap, want] : expect)
  {
    CAPTURE(int(mode), wrap);
    Host h;
    auto& in = h.g->inputs;
    in.scan.value = 1.f;
    in.scan_mode.value = mode;
    in.loopmode.value = wrap;
    in.density.value = 0.5f;
    h.run(0.0001);
    h.forget();
    h.run(1.5);
    // The grain that starts nearest 1.25 s after the start
    const auto segs = h.segments();
    REQUIRE(!segs.empty());
    auto best = std::ranges::min_element(
        segs, {}, [](const Segment& s) { return std::abs(s.start - 1.25 * rate); });
    CAPTURE(best->start);
    const double drift = (1.25 * rate - best->start) / rate; // Scan since
    const double dir = mode == scan_mode::Straight ? 0. : (mode == scan_mode::PingPong ? -1. : 1.);
    CHECK(best->rel == Catch::Approx(want - dir * drift).margin(0.02));
  }
}

TEST_CASE("Scan in folder: Restart from Position, Continue on, Per file where that file was", "[scan][folder]")
{
  for(auto mode : {scan_in::Restart, scan_in::Continue, scan_in::PerFile})
  {
    CAPTURE(int(mode));
    Host h;
    Folder f{3, rate / 2};
    h.use(f, order::Single, "0");
    auto& in = h.g->inputs;
    in.scan.value = 1.f;
    in.dur.value = 0.02f;
    in.scan_folder.value = mode;
    h.run(0.0001);
    h.forget();
    h.run(0.2);
    const long to2 = h.now;
    in.sound_file.value = "2";
    h.run(0.1);
    const long to0 = h.now;
    in.sound_file.value = "0";
    h.run(0.1);
    const auto segs = h.segments();
    REQUIRE(segs.size() > 15);
    // Scan goes through the half-second files at their speed
    const auto went = [](long from, long to) { return double(to - from) / (rate / 2.); };
    std::map<int, const Segment*> last_of;
    int checked = 0;
    for(std::size_t k = 1; k < segs.size(); k++)
    {
      const auto& g = segs[k];
      const auto& before = segs[k - 1];
      CAPTURE(k, g.tag, g.start);
      last_of[before.tag] = &before;
      double want{};
      switch(mode)
      {
        case scan_in::Restart: {
          // From Position at the change of file
          const long from = g.start > to0 ? to0 : g.start > to2 ? to2 : -1;
          want = from < 0 || before.tag == g.tag ? before.rel + went(before.start, g.start)
                                                 : 0.1 + went(from, g.start);
          break;
        }
        case scan_in::Continue:
          want = before.rel + went(before.start, g.start);
          break;
        case scan_in::PerFile: {
          // Its own place, on by as far as Scan went since the grain before
          const auto it = last_of.find(g.tag);
          want = (it != last_of.end() ? it->second->rel : 0.1) + went(before.start, g.start);
          break;
        }
      }
      CHECK(g.rel == Catch::Approx(want).margin(0.002));
      checked++;
    }
    CHECK(checked > 15);
  }
}

TEST_CASE("Scan in folder Per file: a file's place moves only with its own grains", "[scan][folder]")
{
  // Two files in turn: each goes half as fast as Scan
  Host h;
  Folder f{2, rate};
  h.use(f, order::Alphabetical);
  auto& in = h.g->inputs;
  in.scan.value = 0.2f;
  in.dur.value = 0.01f;
  in.scan_folder.value = scan_in::PerFile;
  h.forget();
  h.run(2.);
  for(int tag : {1, 2})
  {
    CAPTURE(tag);
    std::vector<Segment> mine;
    for(auto& s : h.segments())
      if(s.tag == tag)
        mine.push_back(s);
    REQUIRE(mine.size() > 20);
    const double v = (mine.back().rel - mine.front().rel)
                     / (double(mine.back().start - mine.front().start) / rate);
    CHECK(v == Catch::Approx(0.1).margin(0.01));
  }
}

TEST_CASE("Scan in folder Per file: a file's place follows it when the folder is listed again", "[scan][folder]")
{
  Host h;
  Folder f{2, rate};
  h.use(f, order::Single, "f1.wav");
  auto& in = h.g->inputs;
  in.scan.value = 0.2f;
  in.dur.value = 0.01f;
  in.scan_folder.value = scan_in::PerFile;
  h.run(0.5); // f1 at 0.1 + 0.1
  f.add("a.wav", 9, rate, 0);
  f.list_into(*h.g);
  h.forget();
  h.run(0.05);
  const auto segs = h.segments();
  REQUIRE(!segs.empty());
  CHECK(segs.front().tag == 2);
  CHECK(segs.front().rel == Catch::Approx(0.2).margin(0.01));
}

TEST_CASE("Scan's play zone: the panel is told where Scan is, whichever files the grains play", "[scan][ui]")
{
  // The file shown plays one grain in 20: the zone still moves, 30 times a
  // second, as far each time as Scan went
  Host h;
  Folder f{20, rate};
  h.use(f, order::Random);
  h.g->inputs.scan.value = 0.25f;
  h.g->inputs.dur.value = 0.01f;
  h.forget();
  h.run(2.);
  std::vector<std::pair<long, double>> zone;
  for(std::size_t k = 0; k < h.heads.size(); k++)
    if(h.heads[k].scanning)
      zone.emplace_back(h.heads_at[k], h.heads[k].scan / G::playheads::pos_scale);
  REQUIRE(zone.size() > 50);
  for(std::size_t k = 1; k < zone.size(); k++)
  {
    const double want = 0.25 * double(zone[k].first - zone[k - 1].first) / rate;
    const double d = zone[k].second - zone[k - 1].second;
    CAPTURE(k, d, want);
    CHECK((std::abs(d - want) < 0.003 || d < -0.5));
  }

  // Where it says is where the grains of the shown file start
  Host s;
  s.g->inputs.scan.value = 0.25f;
  s.g->inputs.dur.value = 0.01f;
  s.forget();
  s.run(1.);
  const auto segs = s.segments();
  REQUIRE(segs.size() > 10);
  int compared = 0;
  for(std::size_t k = 0; k < s.heads.size(); k++)
  {
    const long at = s.heads_at[k];
    auto near = std::ranges::min_element(segs, {}, [&](auto& g) { return std::abs(g.start - at); });
    if(std::abs(near->start - at) > 480)
      continue;
    CHECK(s.heads[k].scan / G::playheads::pos_scale == Catch::Approx(near->rel).margin(0.01));
    compared++;
  }
  CHECK(compared > 5);
}

// ---------------------------------------------------------------- On MIDI

TEST_CASE("On MIDI Nothing: notes play nothing and transpose nothing", "[midi]")
{
  Host h;
  h.g->inputs.on_midi.value = on_midi::Nothing;
  h.g->inputs.dur.value = 0.02f;
  h.continuous(false);
  h.run(0.1);
  h.forget();
  h.press(72);
  h.run(0.3);
  CHECK(h.silent());
  h.continuous(true);
  const auto segs = heard(h, 0.3);
  CHECK(all_near(slopes(segs), 1., 0.002));
}

TEST_CASE("On MIDI Trigger: a held note plays grains at Pitch, not transposed, none once released", "[midi]")
{
  Host h;
  h.g->inputs.on_midi.value = on_midi::Trigger;
  h.g->inputs.rate.value = 1.5f;
  h.g->inputs.dur.value = 0.02f;
  h.continuous(false);
  h.run(0.1);
  CHECK(h.silent());
  for(int n : {48, 72, 60})
  {
    CAPTURE(n);
    const auto segs = note_heard(h, n, 0.2);
    REQUIRE(segs.size() >= 4);
    CHECK(all_near(slopes(segs), 1.5, 0.003));
    h.forget();
    h.run(0.2);
    CHECK(h.silent());
  }
}

TEST_CASE("On MIDI Trigger and set pitch: a held note plays grains at its pitch, at the pace its pitch makes", "[midi]")
{
  for(auto [n, ratio] : std::vector<std::pair<int, double>>{{72, 2.}, {48, 0.5}, {60, 1.}, {67, std::exp2(7. / 12.)}})
  {
    CAPTURE(n);
    Host h;
    h.g->inputs.on_midi.value = on_midi::TriggerPitch;
    h.g->inputs.dur.value = 0.02f;
    h.continuous(false);
    const auto segs = note_heard(h, n, 0.5);
    REQUIRE(segs.size() >= 3);
    CHECK(all_near(slopes(segs), ratio, 0.003 * ratio));
    // 960 samples of the sound a grain, a grain's length between two
    for(std::size_t k = 1; k < segs.size(); k++)
      CHECK(segs[k].start - segs[k - 1].start == Catch::Approx(1920. / ratio).margin(1.5));
    h.forget();
    h.run(0.2);
    CHECK(h.silent());
  }
}

TEST_CASE("On MIDI Set pitch: Continuous's grains at the pitch of the last note, kept once released", "[midi]")
{
  Host h;
  h.g->inputs.on_midi.value = on_midi::SetPitch;
  h.g->inputs.dur.value = 0.02f;
  CHECK(all_near(slopes(heard(h, 0.2, 0.)), 1., 0.002));
  h.press(67);
  CHECK(all_near(slopes(heard(h, 0.2)), std::exp2(7. / 12.), 0.003));
  h.press(72);
  CHECK(all_near(slopes(heard(h, 0.2)), 2., 0.004));
  // The one held before, then none held: the last one
  h.release(72);
  CHECK(all_near(slopes(heard(h, 0.2)), std::exp2(7. / 12.), 0.003));
  h.release(67);
  CHECK(all_near(slopes(heard(h, 0.3)), std::exp2(7. / 12.), 0.003));
  // No grain of the notes themselves
  h.continuous(false);
  h.run(0.1);
  h.forget();
  h.press(60);
  h.run(0.3);
  CHECK(h.silent());
}

TEST_CASE("On MIDI Set position: Continuous's grains at the note's place in the range, kept once released", "[midi][position][range]")
{
  struct expect
  {
    int low, high;
    std::vector<std::pair<int, double>> notes; // note, place
  };
  const std::vector<expect> cases{
      // The default range, 25 keys: Low at the start, High at the last slice
      {48, 72, {{48, 0.}, {72, 24. / 25.}, {60, 12. / 25.}, {30, 0.}, {100, 24. / 25.}}},
      {60, 83, {{66, 6. / 24.}, {61, 1. / 24.}, {83, 23. / 24.}}},
      {72, 48, {{48, 0.}, {72, 24. / 25.}}},
      {60, 60, {{60, 0.}, {72, 0.}, {20, 0.}}}};
  for(auto& c : cases)
    for(auto [note, place] : c.notes)
    {
      CAPTURE(c.low, c.high, note);
      Host h;
      auto& in = h.g->inputs;
      in.on_midi.value = on_midi::SetPosition;
      in.low_note.value = c.low;
      in.high_note.value = c.high;
      in.pos.value = 0.f;
      in.dur.value = 0.02f;
      // In a region: the place is a part of it
      in.sample_start.value = 0.2f;
      in.sample_end.value = 0.7f;
      h.press(note);
      auto segs = heard(h, 0.2);
      REQUIRE(segs.size() > 3);
      CHECK(all_near(rels(segs), 0.2 + 0.5 * place, 0.002));
      h.release(note);
      segs = heard(h, 0.2);
      REQUIRE(segs.size() > 3);
      CHECK(all_near(rels(segs), 0.2 + 0.5 * place, 0.002));
    }
  // No grain of the notes themselves
  Host h;
  h.g->inputs.on_midi.value = on_midi::SetPosition;
  h.continuous(false);
  h.run(0.1);
  h.forget();
  h.press(60);
  h.run(0.3);
  CHECK(h.silent());
}

TEST_CASE("On MIDI Trigger and set position: each held note plays its own slice, at Pitch", "[midi][position][range]")
{
  Host h;
  auto& in = h.g->inputs;
  in.on_midi.value = on_midi::TriggerPosition;
  in.low_note.value = 60;
  in.high_note.value = 83;
  in.pos.value = 0.f;
  in.dur.value = 0.01f;
  in.rate.value = 0.75f;
  in.density.value = 0.25f;
  h.continuous(false);
  // Their grains play at once: seen as they start, a sample at a time
  h.block = 1;
  h.forget();
  h.press(61);
  h.press(70);
  h.run(0.5);
  h.release(61);
  h.release(70);
  h.run(0.2);
  REQUIRE(h.started.size() > 8);
  std::map<long, int> places;
  for(auto& s : h.started)
  {
    const long k = std::lround(s.pos * 24.);
    places[k]++;
    CHECK(s.pos == Catch::Approx(k / 24.).margin(2e-4));
    CHECK(s.speed == Catch::Approx(0.75).margin(1e-3));
  }
  REQUIRE(places.size() == 2);
  CHECK(places.contains(1));
  CHECK(places.contains(10));
  h.block = 64;
  h.forget();
  h.run(0.3);
  CHECK(h.silent());
}

TEST_CASE("On MIDI with a note's place: Position and Scan are offsets from it", "[midi][position][offset]")
{
  const auto host = [](on_midi m) {
    auto h = std::make_unique<Host>();
    auto& in = h->g->inputs;
    in.on_midi.value = m;
    in.low_note.value = 60;
    in.high_note.value = 83;
    in.dur.value = 0.01f;
    return h;
  };
  SECTION("Position moves the note's place by its amount, latched too")
  {
    auto h = host(on_midi::SetPosition);
    h->g->inputs.pos.value = 0.2f;
    h->press(66);
    CHECK(all_near(rels(heard(*h, 0.2)), 0.25 + 0.2, 0.002));
    h->release(66);
    CHECK(all_near(rels(heard(*h, 0.2)), 0.25 + 0.2, 0.002));
    // Position 0: the note's place itself
    h->g->inputs.pos.value = 0.f;
    CHECK(all_near(rels(heard(*h, 0.2)), 0.25, 0.002));
  }
  SECTION("Scan moves on from the note's place, across notes, a note resetting nothing")
  {
    auto h = host(on_midi::SetPosition);
    auto& in = h->g->inputs;
    in.pos.value = 0.f;
    in.scan.value = 0.25f;
    h->press(62); // 2 / 24
    h->forget();
    h->run(0.4);
    auto segs = h->segments();
    REQUIRE(segs.size() > 10);
    // From the note's place, at Scan's speed
    const double v = (segs.back().rel - segs.front().rel)
                     / (double(segs.back().start - segs.front().start) / rate);
    CHECK(v == Catch::Approx(0.25).margin(0.005));
    const auto& last = segs.back();
    // Another note: its place, plus as far as Scan went
    h->press(68); // 8 / 24
    h->forget();
    h->run(0.1);
    segs = h->segments();
    REQUIRE(!segs.empty());
    const double went = 0.25 * double(segs.front().start - last.start) / rate;
    CHECK(segs.front().rel == Catch::Approx(last.rel + 6. / 24. + went).margin(0.003));
  }
  SECTION("Past the region's end: around, back or held as Scan Mode says")
  {
    for(auto [mode, want] : std::vector<std::pair<scan_mode, double>>{
            {scan_mode::Loop, 23. / 24. + 0.2 - 1.},
            {scan_mode::PingPong, 2. - (23. / 24. + 0.2)},
            {scan_mode::Straight, 1.}})
    {
      CAPTURE(int(mode));
      auto h = host(on_midi::SetPosition);
      h->g->inputs.scan_mode.value = mode;
      h->g->inputs.pos.value = 0.2f;
      h->press(83);
      const auto segs = heard(*h, 0.2);
      REQUIRE(!segs.empty());
      CHECK(all_near(rels(segs), want, 0.002));
    }
  }
  SECTION("Trigger and set position: each note its own place plus the same offset")
  {
    auto h = host(on_midi::TriggerPosition);
    h->g->inputs.pos.value = 0.3f;
    h->g->inputs.density.value = 0.25f;
    h->continuous(false);
    h->block = 1;
    h->forget();
    h->press(61);
    h->press(70);
    h->run(0.5);
    REQUIRE(h->started.size() > 8);
    std::set<long> places;
    for(auto& s : h->started)
    {
      const double k = (s.pos - 0.3) * 24.;
      CHECK(k == Catch::Approx(std::round(k)).margin(5e-3));
      places.insert(std::lround(k));
    }
    CHECK(places == std::set<long>{1, 10});
  }
  SECTION("The play zone shows where the grains start")
  {
    auto h = host(on_midi::SetPosition);
    h->g->inputs.pos.value = 0.2f;
    h->press(66);
    h->run(0.2);
    REQUIRE(!h->heads.empty());
    CHECK(h->heads.back().scanning);
    CHECK(h->heads.back().scan / G::playheads::pos_scale == Catch::Approx(0.45).margin(0.002));
  }
  SECTION("The other modes: Position as before")
  {
    for(auto m : {on_midi::Nothing, on_midi::SetPitch, on_midi::TriggerPitch})
    {
      CAPTURE(int(m));
      auto h = host(m);
      h->g->inputs.pos.value = 0.2f;
      h->press(66);
      const auto segs = heard(*h, 0.2);
      REQUIRE(!segs.empty());
      CHECK(all_near(rels(segs), 0.2, 0.002));
    }
  }
}

TEST_CASE("On MIDI's latch: a file picked by hand or another Sound Order ends it, stopping and starting keep it", "[midi]")
{
  const double up7 = std::exp2(7. / 12.);
  Host h;
  Folder f{3};
  h.use(f, order::Single, "0");
  h.g->inputs.on_midi.value = on_midi::SetPitch;
  h.g->inputs.dur.value = 0.2f;
  const auto latch = [&] {
    h.press(67);
    h.run(0.05);
    h.release(67);
    REQUIRE(all_near(slopes(heard(h, 0.3)), up7, 0.004));
  };
  latch();
  h.g->inputs.sound_file.value = "1";
  CHECK(all_near(slopes(heard(h, 0.3)), 1., 0.002));
  latch();
  h.g->inputs.order.value = order::Alphabetical;
  CHECK(all_near(slopes(heard(h, 0.3)), 1., 0.002));
  latch();
  h.continuous(false);
  h.run(0.2);
  h.continuous(true);
  CHECK(all_near(slopes(heard(h, 0.3)), up7, 0.004));
}

TEST_CASE("By MIDI note whatever On MIDI says: with Nothing, notes pick the file of Continuous", "[midi][order]")
{
  Host h;
  Folder f{4};
  h.use(f, order::ByNote);
  h.g->inputs.on_midi.value = on_midi::Nothing;
  CHECK(file_set(heard(h, 0.2, 0.)) == std::set{0});
  h.press(64); // file 2 of 4 over 48 to 72
  auto segs = heard(h, 0.2);
  CHECK(file_set(segs) == std::set{2});
  CHECK(all_near(slopes(segs), 1., 0.002));
  h.release(64);
  CHECK(file_set(heard(h, 0.2)) == std::set{2});
  // And no grain of the notes
  h.continuous(false);
  h.run(0.1);
  h.forget();
  h.press(72);
  h.run(0.2);
  CHECK(h.silent());
}

TEST_CASE("Velocity: a note's grains at 1 - amount (1 - (velocity / 127)^2), each from its start", "[midi][velocity]")
{
  const auto level = [](float amount, int vel, on_midi mode) {
    Host h{1., 1, 0.5f};
    h.g->inputs.velocity.value = amount;
    h.g->inputs.on_midi.value = mode;
    h.g->inputs.dur.value = 0.02f;
    const bool set = mode == on_midi::SetPitch || mode == on_midi::SetPosition;
    h.continuous(set);
    h.run(0.1);
    h.press(60, vel);
    if(set)
    {
      // Released: the latched note's velocity still
      h.run(0.01);
      h.release(60);
    }
    h.run(0.1);
    h.forget();
    h.run(0.3);
    std::set<long> res;
    for(double l : h.levels())
      res.insert(std::lround(l / 0.5 * 1e4));
    return res;
  };
  const double c64 = (64. / 127.) * (64. / 127.);
  const auto q = [](double v) { return std::set<long>{std::lround(v * 1e4)}; };
  CHECK(level(0.f, 64, on_midi::Trigger) == q(1.));
  CHECK(level(1.f, 127, on_midi::Trigger) == q(1.));
  CHECK(level(1.f, 64, on_midi::Trigger) == q(c64));
  CHECK(level(0.5f, 64, on_midi::TriggerPitch) == q(1. - 0.5 * (1. - c64)));
  CHECK(level(1.f, 32, on_midi::TriggerPosition) == q((32. / 127.) * (32. / 127.)));
  CHECK(level(1.f, 64, on_midi::SetPitch) == q(c64));
  CHECK(level(0.25f, 64, on_midi::SetPosition) == q(1. - 0.25 * (1. - c64)));

  // A grain keeps the level it starts with: another note mid-grain sets the
  // next grains' level, not its own
  Host h{1., 1, 0.5f};
  h.g->inputs.on_midi.value = on_midi::SetPitch;
  h.g->inputs.dur.value = 0.3f;
  h.continuous(false);
  h.forget();
  h.run(0.01);
  h.press(60, 127);
  h.continuous(true);
  h.run(0.05);
  h.press(62, 64);
  h.run(1.5);
  const auto runs = h.runs();
  REQUIRE(runs.size() >= 2);
  for(auto& r : runs)
  {
    const auto [lo, hi] = std::ranges::minmax(r);
    CHECK(lo == hi);
  }
  CHECK(runs[0].front() / 0.5 == Catch::Approx(1.).margin(1e-6));
  CHECK(runs[1].front() / 0.5 == Catch::Approx(c64).margin(1e-6));
}

// ---------------------------------------------------------------- Keep time

TEST_CASE("Keep time: a grain lasts Duration at any pitch, reading as much faster", "[keep-time]")
{
  for(float pitch : {0.5f, 1.f, 2.f})
    for(bool keep : {false, true})
    {
      CAPTURE(pitch, keep);
      Host h{2.};
      h.g->inputs.keep_time.value = keep;
      h.g->inputs.rate.value = pitch;
      const auto segs = heard(h, 1.5, 0.);
      REQUIRE(segs.size() >= 3);
      // 0.05 of 2 s: 4800 samples at speed 1
      const double want = keep ? 4800. : 4800. / pitch;
      for(auto& s : segs)
      {
        CHECK(s.length == Catch::Approx(want).margin(2));
        CHECK(s.slope == Catch::Approx(pitch).margin(0.002));
      }
      // As often at any pitch: a grain's length, then as long without one
      for(std::size_t k = 1; k < segs.size(); k++)
        CHECK(segs[k].start - segs[k - 1].start == Catch::Approx(2. * want).margin(1.5));
    }
}

TEST_CASE("Keep time: notes of any pitch play their grains at the same instants", "[keep-time][midi]")
{
  for(bool keep : {true, false})
  {
    CAPTURE(keep);
    Host h;
    h.g->inputs.keep_time.value = keep;
    h.g->inputs.on_midi.value = on_midi::TriggerPitch;
    h.continuous(false);
    h.press(48);
    h.press(72);
    h.run(1.);
    std::vector<long> starts;
    for(auto& s : h.started)
      starts.push_back(s.at);
    std::ranges::sort(starts);
    REQUIRE(starts.size() >= 8);
    bool paired = starts.size() % 2 == 0;
    for(std::size_t k = 0; paired && k + 1 < starts.size(); k += 2)
      paired = starts[k + 1] - starts[k] <= 1;
    CHECK(paired == keep);
  }
}

TEST_CASE("Keep time: a fast grain ends at the region's end, or with Wrap at end reads it once at most", "[keep-time]")
{
  for(bool wrap : {false, true})
  {
    CAPTURE(wrap);
    Host h;
    auto& in = h.g->inputs;
    in.keep_time.value = true;
    in.rate.value = 8.f;
    in.pos.value = 0.8f;
    in.dur.value = wrap ? 0.5f : 0.1f;
    in.loopmode.value = wrap;
    in.density.value = 0.25f;
    h.forget();
    h.run(5.);
    REQUIRE(!h.runs().empty());
    for(auto& s : h.runs())
      CHECK(double(s.size()) == Catch::Approx(wrap ? rate / 8. : 0.2 * rate / 8.).margin(3));
  }
}

// ---------------------------------------------------------------- Grain clock

TEST_CASE("Density: grains a grain's length apart divided by it, past whole samples", "[timing]")
{
  for(float d : {0.5f, 2.f, 4.f, 8.f})
  {
    CAPTURE(d);
    Host h;
    h.g->inputs.density.value = d;
    h.run(1.);
    std::vector<long> at;
    for(auto& s : h.started)
      at.push_back(s.at);
    REQUIRE(at.size() > 4);
    for(std::size_t k = 1; k < at.size(); k++)
      CHECK(at[k] - at[k - 1] == Catch::Approx(2400. / d).margin(1));
  }
  // An interval of 1.5 samples: as many grains, not one every 2
  Host h;
  h.block = 16;
  h.g->inputs.dur.value = 24.f / rate;
  h.g->inputs.density.value = 16.f;
  h.run(0.1);
  h.forget();
  h.run(1.);
  CHECK(double(h.started.size()) == Catch::Approx(rate / 1.5).epsilon(0.01));
}

TEST_CASE("Density is sample-accurate; 0 makes no grain, and any value above it one at once", "[timing]")
{
  Host h;
  h.block = 512;
  h.g->inputs.density.value = 0.f;
  h.run(0.5);
  CHECK(h.silent());
  CHECK(h.started.empty());
  // A value at sample 300 of a tick: the first grain there
  const long tick_at = h.now;
  h.g->inputs.density.values[300] = 4.f;
  h.tick();
  h.g->inputs.density.value = 4.f;
  h.run(0.2);
  REQUIRE(!h.started.empty());
  CHECK(h.started.front().at == tick_at + 300);
  // From 4 to 1 at sample 100 of a tick: the next interval from there
  h.forget();
  h.g->inputs.density.values[100] = 1.f;
  const long changed = h.now + 100;
  h.tick();
  h.g->inputs.density.value = 1.f;
  h.run(0.5);
  REQUIRE(h.started.size() >= 2);
  for(std::size_t k = 1; k < h.started.size(); k++)
    if(h.started[k - 1].at >= changed)
      CHECK(h.started[k].at - h.started[k - 1].at == Catch::Approx(2400.).margin(1));
}

TEST_CASE("Density Deviation: intervals 2^N(0, d / 2) times as long, as many shorter as longer", "[timing]")
{
  for(float dev : {0.4f, 1.f})
  {
    CAPTURE(dev);
    Host h;
    h.g->inputs.density.value = 4.f;
    h.g->inputs.dens_j.value = dev;
    h.run(12.);
    std::vector<double> oct;
    for(std::size_t k = 1; k < h.started.size(); k++)
      oct.push_back(std::log2(double(h.started[k].at - h.started[k - 1].at) / 600.));
    REQUIRE(oct.size() > 500);
    CHECK(mean(oct) == Catch::Approx(0.).margin(0.06));
    CHECK(stddev(oct) == Catch::Approx(dev / 2.).epsilon(0.12));
    for(double o : oct)
      CHECK(std::abs(o) <= 4. + 1e-2);
  }
}

TEST_CASE("Max Voices: never more grains at once; 0 none; past 256, 256", "[timing]")
{
  for(auto [voices, most] : std::vector<std::pair<int, int>>{{3, 3}, {1, 1}, {0, 0}, {256, 256}, {300, 256}})
  {
    CAPTURE(voices);
    Host h;
    h.g->inputs.density.value = 256.f;
    h.g->inputs.dur.value = 0.5f;
    h.voices(voices);
    h.run(1.);
    CHECK(h.most_active == most);
    CHECK(h.silent() == (most == 0));
  }
}

TEST_CASE("Continuous and Trigger: grains on their own, or one a Trigger", "[timing]")
{
  Host h;
  h.continuous(false);
  h.run(0.3);
  CHECK(h.silent());
  for(int k = 0; k < 3; k++)
  {
    h.forget();
    h.run(0.01);
    h.trigger();
    h.run(0.3);
    CHECK(h.segments().size() == 1);
  }
  // Continuous again, after a while: a grain at once, the next an interval
  // later
  for(double stopped : {0.01, 0.033, 0.3})
  {
    CAPTURE(stopped);
    h.continuous(false);
    h.run(stopped);
    h.forget();
    const long on = h.now;
    h.continuous(true);
    h.run(0.5);
    REQUIRE(h.started.size() >= 4);
    CHECK(h.started[0].at == on);
    for(std::size_t k = 1; k < h.started.size(); k++)
      CHECK(h.started[k].at - h.started[k - 1].at == Catch::Approx(4800).margin(1));
  }
}

// ---------------------------------------------------------------- Grain

TEST_CASE("Position and Duration: where grains start and how much they read, parts of the region", "[grain]")
{
  for(auto [lo, hi] : std::vector<std::pair<float, float>>{{0.f, 1.f}, {0.2f, 0.6f}, {0.6f, 0.2f}})
  {
    CAPTURE(lo, hi);
    Host h;
    auto& in = h.g->inputs;
    in.pos.value = 0.3f;
    in.dur.value = 0.1f;
    in.sample_start.value = lo;
    in.sample_end.value = hi;
    const auto segs = heard(h, 1., 0.);
    REQUIRE(segs.size() >= 3);
    const double a = std::min(lo, hi), len = std::abs(hi - lo);
    for(auto& s : segs)
    {
      CHECK(s.rel == Catch::Approx(a + 0.3 * len).margin(1e-3));
      CHECK(s.length == Catch::Approx(0.1 * len * rate).margin(2));
    }
  }
}

TEST_CASE("Position and Duration Deviations: N(0, d / 4) of the region around them", "[grain]")
{
  Host h{0.25};
  auto& in = h.g->inputs;
  in.pos.value = 0.4f;
  in.dur.value = 0.2f;
  in.pos_j.value = 0.4f;
  in.dur_j.value = 0.2f;
  const auto segs = heard(h, 60., 0.);
  REQUIRE(segs.size() > 250);
  std::vector<double> starts, lengths;
  for(auto& s : segs)
  {
    starts.push_back(s.rel);
    lengths.push_back(double(s.length) / (0.25 * rate));
  }
  CHECK(mean(starts) == Catch::Approx(0.4).margin(0.015));
  CHECK(stddev(starts) == Catch::Approx(0.1).epsilon(0.12));
  CHECK(mean(lengths) == Catch::Approx(0.2).margin(0.01));
  CHECK(stddev(lengths) == Catch::Approx(0.05).epsilon(0.12));
}

TEST_CASE("Sample Start and End: no grain reads outside of the region, whatever moves them", "[grain][region]")
{
  Host h;
  auto& in = h.g->inputs;
  in.sample_start.value = 0.6f;
  in.sample_end.value = 0.3f;
  in.pos_j.value = 1.f;
  in.dur.value = 0.5f;
  in.dur_j.value = 1.f;
  in.reverse_chance.value = 0.5f;
  in.scan.value = 1.3f;
  in.rate.value = 1.7f;
  in.rate_j.value = 1.f;
  h.voices(1);
  h.forget();
  h.run(5.);
  const double lo = 0.3 * (rate - 1) - 1., hi = 0.6 * (rate - 1) + 1.;
  long read = 0;
  for(double v : h.rec[0])
    if(v != 0.)
    {
      const double i = (v - sound_tag) * 2. * rate;
      read++;
      if(i < lo || i > hi)
      {
        CAPTURE(i);
        CHECK(false);
        break;
      }
    }
  CHECK(read > rate);
}

TEST_CASE("Wrap at end: a grain past the region's end goes on from its start", "[grain][region]")
{
  for(bool wrap : {false, true})
  {
    CAPTURE(wrap);
    Host h;
    auto& in = h.g->inputs;
    in.sample_start.value = 0.2f;
    in.sample_end.value = 0.6f;
    in.pos.value = 0.9f; // 0.56
    in.dur.value = 0.3f; // 0.12
    in.loopmode.value = wrap;
    in.density.value = 0.25f;
    h.forget();
    h.run(1.);
    const auto runs = h.runs();
    REQUIRE(!runs.empty());
    for(auto& r : runs)
    {
      CHECK(double(r.size()) == Catch::Approx(wrap ? 0.12 * rate : 0.04 * rate).margin(2));
      // The last sample read: past the start of the region, with Wrap at end
      const double last = (r[r.size() - 2] - sound_tag) * 2. * rate / (rate - 1);
      CHECK(last == Catch::Approx(wrap ? 0.28 : 0.6).margin(0.002));
    }
  }
}

TEST_CASE("Reverse Chance: as many grains backwards", "[grain][pitch]")
{
  for(float chance : {0.f, 0.25f, 0.75f, 1.f})
  {
    CAPTURE(chance);
    Host h;
    h.g->inputs.reverse_chance.value = chance;
    h.g->inputs.pos.value = 0.5f;
    h.g->inputs.dur.value = 0.005f;
    const auto segs = heard(h, 40., 0.);
    REQUIRE(segs.size() > 300);
    int back = 0;
    for(auto& s : segs)
    {
      CHECK(std::abs(s.slope) == Catch::Approx(1.).margin(0.01));
      back += s.slope < 0.;
    }
    CHECK(double(back) / segs.size() == Catch::Approx(chance).margin(0.06));
  }
}

TEST_CASE("Pitch, Pitch Deviation, in Semitones: how fast each grain reads", "[grain][pitch]")
{
  for(float p : {0.25f, 0.75f, 1.f, 3.f})
  {
    CAPTURE(p);
    Host h;
    h.g->inputs.rate.value = p;
    h.g->inputs.dur.value = 0.02f;
    CHECK(all_near(slopes(heard(h, 1., 0.)), p, 0.002 * p));
  }
  // As a ratio: Pitch + N(0, d / 4)
  {
    Host h;
    h.g->inputs.rate.value = 1.f;
    h.g->inputs.rate_j.value = 0.4f;
    h.g->inputs.dur.value = 0.005f;
    h.g->inputs.density.value = 0.2f;
    const auto s = slopes(heard(h, 30., 0.));
    REQUIRE(s.size() > 300);
    CHECK(mean(s) == Catch::Approx(1.).margin(0.015));
    CHECK(stddev(s) == Catch::Approx(0.1).epsilon(0.12));
  }
  // In semitones: whole ones, both ways, 3 of deviation at 0.5
  {
    Host h;
    h.g->inputs.rate.value = 1.f;
    h.g->inputs.rate_j.value = 0.5f;
    h.g->inputs.rate_j_semitones.value = true;
    h.g->inputs.dur.value = 0.005f;
    h.g->inputs.density.value = 0.1f;
    const auto s = slopes(heard(h, 60., 0.));
    REQUIRE(s.size() > 300);
    std::vector<double> semis;
    for(double v : s)
    {
      const double k = 12. * std::log2(v);
      CHECK(std::abs(k - std::round(k)) < 0.03);
      semis.push_back(std::round(k));
    }
    CHECK(mean(semis) == Catch::Approx(0.).margin(0.4));
    CHECK(stddev(semis) == Catch::Approx(3.).epsilon(0.12));
  }
}

TEST_CASE("Interpolation: None holds samples, Linear and Cubic go between, Cubic closer to a curve", "[grain]")
{
  // A sine to read at 0.37: grain sample k reads the sound at its start
  // plus 0.37 k, from Position, a part of the sound's last sample
  constexpr double period = 32.;
  const auto sine = [](double i) { return 0.5 * std::sin(2. * M_PI * i / period); };
  const auto error = [&](interp mode) {
    Host h;
    h.fill(0, sine);
    h.g->inputs.interp_type.value = mode;
    h.g->inputs.rate.value = 0.37f;
    h.g->inputs.dur.value = 0.005f;
    h.forget();
    h.run(0.5);
    const double start = double(0.1f) * (rate - 1);
    double e = 0.;
    long n = 0;
    for(auto& st : h.started)
    {
      const long at = st.at - h.rec_from;
      if(at < 0)
        continue;
      for(long k = 0; k < 200; k++, n++)
        e = std::max(e, std::abs(h.rec[0][at + k] - sine(start + k * double(0.37f))));
    }
    REQUIRE(n > 1000);
    return e;
  };
  const double none = error(interp::None), linear = error(interp::Linear),
               cubic = error(interp::Cubic);
  CAPTURE(none, linear, cubic);
  // None: off by up to a sample's step of the sine; Linear by the curve
  // between two samples; Cubic far less
  CHECK(none > 0.05);
  CHECK(linear > 1e-3);
  CHECK(linear < 0.003);
  CHECK(cubic < linear / 4.);
}

TEST_CASE("Window: its shape over each grain, peaking at 1, tilted by x", "[grain][window]")
{
  // A constant sound: the output is the window
  const auto envelope = [](window_mode mode, float x, float y) {
    Host h{1., 1, 1.f};
    h.g->inputs.window_mode.value = mode;
    h.g->inputs.win_coefs.value = {x, y};
    h.g->inputs.dur.value = 0.05f;
    h.forget();
    h.run(0.3);
    const auto runs = h.runs();
    REQUIRE(!runs.empty());
    return runs.front();
  };
  for(auto mode : {window_mode::Beta, window_mode::Cos, window_mode::Kuma})
  {
    CAPTURE(int(mode));
    const auto mid = envelope(mode, 0.5f, 0.05f);
    const auto peak = std::ranges::max_element(mid);
    CHECK(*peak == Catch::Approx(1.).margin(0.01));
    CHECK(mid.front() < 0.1);
    CHECK(mid.back() < 0.1);
    CHECK(double(mid.size()) == Catch::Approx(2400.).margin(3));
    if(mode != window_mode::Kuma)
      CHECK(double(peak - mid.begin()) / mid.size() == Catch::Approx(0.5).margin(0.03));
    // Tilted: the peak earlier towards x = 0, later towards x = 1
    const auto early = envelope(mode, 0.15f, 0.1f), late = envelope(mode, 0.85f, 0.1f);
    const double at_early = double(std::ranges::max_element(early) - early.begin()) / early.size();
    const double at_late = double(std::ranges::max_element(late) - late.begin()) / late.size();
    CAPTURE(at_early, at_late);
    CHECK(at_early < 0.4);
    CHECK(at_late > 0.6);
    // At the bottom of the pad: flat
    for(double v : envelope(mode, 0.5f, 0.f))
      CHECK(v == Catch::Approx(1.).margin(1e-6));
  }
}

TEST_CASE("Gain and Gain Deviation: the level, some grains lower and none louder", "[grain][output]")
{
  for(float g : {0.25f, 1.f, 3.f})
  {
    CAPTURE(g);
    Host h{1., 1, 0.5f};
    h.g->inputs.gain.value = g;
    for(double l : (h.run(0.5), h.levels()))
      CHECK(l == Catch::Approx(0.5 * g).epsilon(1e-6));
  }
  Host h{1., 1, 0.5f};
  h.g->inputs.gain_j.value = 1.f;
  h.g->inputs.dur.value = 0.005f;
  h.forget();
  h.run(20.);
  std::vector<double> db;
  for(double l : h.levels())
  {
    CHECK(l <= 0.5 + 1e-9);
    db.push_back(20. * std::log10(l / 0.5));
  }
  REQUIRE(db.size() > 300);
  std::ranges::sort(db);
  // -24 |N(0, 1)| dB: its median 0.674 of 24 down
  CHECK(db[db.size() / 2] == Catch::Approx(-0.674 * 24.).margin(1.5));
}

TEST_CASE("Spread: in every output alike, between two neighbours at the same power, or in one", "[grain][output]")
{
  for(float spread : {0.f, 0.5f, 1.f, 1.5f, 2.f})
  {
    CAPTURE(spread);
    Host h{1., 1, 1.f};
    h.g->inputs.out_channels.value = 4;
    h.g->inputs.spread.value = spread;
    h.g->inputs.dur.value = 0.005f;
    h.run(0.01);
    h.forget();
    h.run(3.);
    REQUIRE(h.rec.size() >= 4);
    std::set<int> loudest;
    int grains = 0;
    for(auto& s : h.started)
    {
      const long mid = s.at + 100 - h.rec_from;
      if(mid >= (long)h.rec[0].size())
        continue;
      grains++;
      std::array<double, 4> l;
      for(int c = 0; c < 4; c++)
        l[c] = h.rec[c][mid];
      const double all = std::ranges::min(l);
      int lit = 0;
      double pair_power = 0.;
      for(int c = 0; c < 4; c++)
        if(l[c] > all + 1e-6)
        {
          lit++;
          pair_power += (l[c] - all) * (l[c] - all);
        }
      loudest.insert(int(std::ranges::max_element(l) - l.begin()));
      // What every output has: 1 - Spread, up to 1
      CHECK(all == Catch::Approx(std::max(0.f, 1.f - spread)).margin(1e-6));
      CHECK(lit <= 2);
      if(spread == 1.f)
        CHECK(pair_power == Catch::Approx(1.).margin(1e-4));
      if(spread == 2.f)
        CHECK(lit == 1);
    }
    CHECK(grains > 100);
    if(spread > 0.f)
      CHECK(loudest == std::set{0, 1, 2, 3});
  }
}

TEST_CASE("Outputs, Source Channels, Channel Offset: which channels of the sound reach which outputs", "[grain][output]")
{
  struct expect
  {
    int src, offset, outs;
    std::vector<int> tags; // of each output
  };
  for(auto c : std::vector<expect>{
          {0, 0, 0, {20, 21, 22}},
          {1, 2, 0, {22}},
          {2, 1, 0, {21, 22}},
          {2, 1, 5, {21, 22, 21, 22, 21}},
          {3, 2, 0, {20, 21, 22}}})
  {
    CAPTURE(c.src, c.offset, c.outs);
    Host h{1., 3};
    h.g->inputs.src_channels.value = c.src;
    h.g->inputs.channel_offset.value = c.offset;
    h.g->inputs.out_channels.value = c.outs;
    h.run(0.1);
    h.forget();
    h.run(0.5);
    REQUIRE((int)h.g->outputs.audio.channels == (int)c.tags.size());
    for(std::size_t j = 0; j < c.tags.size(); j++)
    {
      CAPTURE(j);
      const auto segs = h.segments(int(j));
      REQUIRE(!segs.empty());
      for(auto& s : segs)
        CHECK(s.tag == c.tags[j]);
    }
  }
}

// ---------------------------------------------------------------- Defaults and ports

TEST_CASE("A new Granola: two outputs, each grain panned between them", "[output][defaults]")
{
  const G fresh;
  REQUIRE(fresh.inputs.out_channels.value == 2);
  REQUIRE(fresh.inputs.spread.value == 1.f);
  // A mono sound with those defaults
  Host h{1., 1, 1.f};
  h.g->inputs.out_channels.value = fresh.inputs.out_channels.value;
  h.g->inputs.spread.value = fresh.inputs.spread.value;
  h.g->inputs.dur.value = 0.005f;
  h.run(0.01);
  h.forget();
  h.run(3.);
  REQUIRE((int)h.g->outputs.audio.channels == 2);
  std::vector<double> balance;
  for(auto& s : h.started)
  {
    const long mid = s.at + 100 - h.rec_from;
    if(mid >= (long)h.rec[0].size())
      continue;
    const double l = h.rec[0][mid], r = h.rec[1][mid];
    // As loud wherever it is
    CHECK(l * l + r * r == Catch::Approx(1.).margin(1e-4));
    balance.push_back(r * r);
  }
  REQUIRE(balance.size() > 100);
  // Anywhere between the two, as often on either side
  CHECK(*std::ranges::min_element(balance) < 0.1);
  CHECK(*std::ranges::max_element(balance) > 0.9);
  CHECK(mean(balance) == Catch::Approx(0.5).margin(0.08));
}

TEST_CASE("Every port says what it does", "[ports]")
{
  int inputs = 0, outputs = 0;
  G g;
  avnd::input_introspection<G>::for_all(g.inputs, [&]<typename F>(F&) {
    CAPTURE(std::string(avnd::get_name<F>()));
    CHECK(avnd::has_description<F>);
    if constexpr(avnd::has_description<F>)
    {
      constexpr std::string_view d = avnd::get_description<F>();
      CHECK(d.size() > 20);
      CHECK(d.ends_with('.'));
      CHECK(std::ranges::all_of(d, [](char c) { return c >= 32 && c < 127; }));
    }
    inputs++;
  });
  avnd::output_introspection<G>::for_all(g.outputs, [&]<typename F>(F&) {
    CAPTURE(std::string(avnd::get_name<F>()));
    CHECK(avnd::has_description<F>);
    outputs++;
  });
  CHECK(inputs == 39);
  CHECK(outputs == 2);
}
