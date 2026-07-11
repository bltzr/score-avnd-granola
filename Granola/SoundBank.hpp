#pragma once

/* Folder-based sound bank + MIDI key-zone mapping for Granola.
 *
 * The bank mirrors a folder of audio files (sorted by name → stable indices).
 * Scanning/decoding runs in a worker thread; unchanged files are reused from
 * the previous bank so a rescan only decodes what changed. Entries are
 * shared_ptr so grains can hold their sound across a bank swap.
 *
 * The MIDI map is a JSON file (typically living in the same folder, but any
 * path relative to it or absolute works):
 *   { "zones": [ { "notes": [36, 47], "sound": "kick.wav", "root": 40 }, ... ] }
 * "notes" is an inclusive [low, high] range (or a single int), "sound" a file
 * name in the bank, "root" the unity-pitch note (default 60).
 */

#include "grain.hpp"

#include <Media/AudioDecoder.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>
#include <string>
#include <vector>

namespace Granola
{

struct BankSound
{
  Media::audio_array data;         // [channel][frame]
  std::vector<const float*> ptrs; // per-channel pointers into data
  std::string name;
  int64_t mtime{};

  // Downsampled min/max envelope over all channels, for the waveform UI.
  // Computed once at scan time (worker thread) and cached with the sound.
  std::vector<float> min_peaks, max_peaks;
  float duration_s{};

  GrainSource view() const noexcept
  {
    return GrainSource{
        ptrs.data(), (long)ptrs.size(),
        double(ptrs.empty() ? 0 : data[0].size())};
  }
};

// Runs in the worker thread, right after decoding.
inline void compute_peaks(BankSound& snd, double rate)
{
  if(snd.data.empty() || snd.data[0].empty() || rate <= 0)
    return;
  const std::size_t frames = snd.data[0].size();
  snd.duration_s = float(frames / rate);

  const std::size_t buckets = std::min<std::size_t>(1024, frames);
  snd.min_peaks.assign(buckets, 0.f);
  snd.max_peaks.assign(buckets, 0.f);
  for(std::size_t b = 0; b < buckets; b++)
  {
    const std::size_t begin = b * frames / buckets;
    const std::size_t end = std::max(begin + 1, (b + 1) * frames / buckets);
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for(const auto& ch : snd.data)
      for(std::size_t i = begin; i < end && i < ch.size(); i++)
      {
        lo = std::min(lo, ch[i]);
        hi = std::max(hi, ch[i]);
      }
    if(lo > hi)
      lo = hi = 0.f;
    snd.min_peaks[b] = lo;
    snd.max_peaks[b] = hi;
  }
}

struct MidiZone
{
  int lo{0}, hi{127};
  int root{60};
  int sound_index{-1}; // resolved against the bank after scan
  std::string sound;
};

struct SoundBank
{
  std::string folder;
  std::vector<std::shared_ptr<const BankSound>> sounds; // sorted by name

  std::string map_file;
  int64_t map_mtime{};
  std::vector<MidiZone> zones;

  int index_of(std::string_view name) const noexcept
  {
    for(std::size_t i = 0; i < sounds.size(); i++)
      if(sounds[i]->name == name)
        return (int)i;
    return -1;
  }

  const MidiZone* zone_for(int note) const noexcept
  {
    for(auto& z : zones)
      if(note >= z.lo && note <= z.hi && z.sound_index >= 0)
        return &z;
    return nullptr;
  }
};

// Runs in the worker thread.
inline SoundBank scan_folder(
    const std::string& folder, const std::string& map_file, double rate,
    const SoundBank& previous)
{
  SoundBank out;
  out.folder = folder;
  out.map_file = map_file;
  if(folder.empty())
    return out;

  QDir dir(QString::fromStdString(folder));
  static const QStringList exts{"*.wav", "*.aif", "*.aiff", "*.flac",
                                "*.mp3", "*.ogg",  "*.m4a"};
  const auto files = dir.entryInfoList(exts, QDir::Files, QDir::Name);
  for(const auto& fi : files)
  {
    const auto name = fi.fileName().toStdString();
    const auto mtime = fi.lastModified().toMSecsSinceEpoch();

    // reuse unchanged sounds from the previous bank
    if(previous.folder == folder)
    {
      const std::shared_ptr<const BankSound>* prev = nullptr;
      for(auto& s : previous.sounds)
        if(s->name == name && s->mtime == mtime)
        {
          prev = &s;
          break;
        }
      if(prev)
      {
        out.sounds.push_back(*prev);
        continue;
      }
    }

    auto dec = Media::AudioDecoder::decode_synchronous(
        fi.absoluteFilePath(), (int)rate);
    if(!dec)
      continue;
    auto snd = std::make_shared<BankSound>();
    snd->data = std::move(dec->second);
    snd->name = name;
    snd->mtime = mtime;
    snd->ptrs.reserve(snd->data.size());
    for(auto& ch : snd->data)
      snd->ptrs.push_back(ch.data());
    compute_peaks(*snd, rate);
    if(!snd->ptrs.empty())
      out.sounds.push_back(std::move(snd));
  }

  // MIDI map: name resolved inside the folder, or absolute path
  if(!map_file.empty())
  {
    QString map_path = QString::fromStdString(map_file);
    if(QFileInfo(map_path).isRelative())
      map_path = dir.filePath(map_path);

    QFile f(map_path);
    out.map_mtime = QFileInfo(map_path).exists()
                        ? QFileInfo(map_path).lastModified().toMSecsSinceEpoch()
                        : 0;
    if(f.open(QIODevice::ReadOnly))
    {
      const auto doc = QJsonDocument::fromJson(f.readAll());
      for(const auto& zv : doc.object()["zones"].toArray())
      {
        const auto zo = zv.toObject();
        MidiZone z;
        if(const auto notes = zo["notes"]; notes.isArray())
        {
          const auto arr = notes.toArray();
          if(arr.size() >= 2)
          {
            z.lo = arr[0].toInt();
            z.hi = arr[1].toInt();
          }
          else if(arr.size() == 1)
            z.lo = z.hi = arr[0].toInt();
        }
        else if(notes.isDouble())
          z.lo = z.hi = notes.toInt();
        z.root = zo["root"].toInt(60);
        z.sound = zo["sound"].toString().toStdString();
        z.sound_index = out.index_of(z.sound);
        out.zones.push_back(std::move(z));
      }
    }
  }

  return out;
}

}
