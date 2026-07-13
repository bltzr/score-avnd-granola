#pragma once

/* Folder-based sound bank for Granola.
 *
 * The bank mirrors a folder of audio files (sorted by name -> stable indices).
 * Scanning/decoding runs in a worker thread; unchanged files are reused from
 * the previous bank so a rescan only decodes what changed. Entries are
 * shared_ptr so grains can hold their sound across a bank swap.
 *
 * The folder is inferred from the Sound port's file: picking any file in a
 * folder loads every file in it, the picked one selectable by "Sound index".
 */

#include "grain.hpp"

#include <Media/AudioDecoder.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <memory>
#include <string>
#include <vector>

namespace Granola
{

struct BankSound
{
  Media::audio_array data;         // [channel][frame]
  std::vector<const float*> ptrs;  // per-channel pointers into data
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

// Downsampled min/max envelope over all channels. Used by the waveform UI.
inline void compute_peaks_raw(
    const float* const* data, int channels, int64_t frames, double rate,
    std::vector<float>& min_peaks, std::vector<float>& max_peaks, float& duration_s)
{
  min_peaks.clear();
  max_peaks.clear();
  duration_s = 0.f;
  if(!data || channels <= 0 || frames <= 0 || rate <= 0)
    return;
  duration_s = float(frames / rate);

  const std::size_t buckets = std::min<std::size_t>(1024, (std::size_t)frames);
  min_peaks.assign(buckets, 0.f);
  max_peaks.assign(buckets, 0.f);
  for(std::size_t b = 0; b < buckets; b++)
  {
    const std::size_t begin = b * frames / buckets;
    const std::size_t end = std::max(begin + 1, (b + 1) * (std::size_t)frames / buckets);
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for(int c = 0; c < channels; c++)
      for(std::size_t i = begin; i < end && i < (std::size_t)frames; i++)
      {
        lo = std::min(lo, data[c][i]);
        hi = std::max(hi, data[c][i]);
      }
    if(lo > hi)
      lo = hi = 0.f;
    min_peaks[b] = lo;
    max_peaks[b] = hi;
  }
}

// Runs in the worker thread, right after decoding.
inline void compute_peaks(BankSound& snd, double rate)
{
  if(snd.data.empty() || snd.data[0].empty() || rate <= 0)
    return;
  compute_peaks_raw(
      snd.ptrs.data(), (int)snd.ptrs.size(), (int64_t)snd.data[0].size(), rate,
      snd.min_peaks, snd.max_peaks, snd.duration_s);
}

struct SoundBank
{
  std::string folder; // raw Sound-port path (file or folder), for change detection
  std::vector<std::shared_ptr<const BankSound>> sounds; // sorted by name

  int index_of(std::string_view name) const noexcept
  {
    for(std::size_t i = 0; i < sounds.size(); i++)
      if(sounds[i]->name == name)
        return (int)i;
    return -1;
  }
};

// Runs in the worker thread.
inline SoundBank
scan_folder(const std::string& folder, double rate, const SoundBank& previous)
{
  SoundBank out;
  out.folder = folder; // raw input path, so the tick's change-detection is stable
  if(folder.empty())
    return out;

  // The Sound input may be a folder or a file: a file resolves to its folder.
  QFileInfo fi_in(QString::fromStdString(folder));
  QDir dir = fi_in.isFile() ? fi_in.absoluteDir()
                            : QDir(QString::fromStdString(folder));
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
    // zero-frame files would give grains a negative buffer length
    if(!snd->ptrs.empty() && !snd->data[0].empty())
      out.sounds.push_back(std::move(snd));
  }

  return out;
}

}
