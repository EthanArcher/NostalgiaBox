#!/usr/bin/env bash
# Bake the NostalgiaBox CRT look into video copies.
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 INPUT_DIR OUTPUT_DIR" >&2
  exit 2
fi

input_dir="${1%/}"
output_dir="${2%/}"

if [[ ! -d "$input_dir" ]]; then
  echo "error: input directory does not exist: $input_dir" >&2
  exit 1
fi
command -v ffmpeg >/dev/null || {
  echo "error: ffmpeg is required (sudo apt install ffmpeg)" >&2
  exit 1
}
command -v ffprobe >/dev/null || {
  echo "error: ffprobe is required (sudo apt install ffmpeg)" >&2
  exit 1
}

progress_file="$(mktemp)"
trap 'rm -f "$progress_file"' EXIT

find "$input_dir" -type f ! -name '._*' \( \
  -iname '*.mp4' -o -iname '*.mkv' -o -iname '*.avi' -o \
  -iname '*.m4v' -o -iname '*.mov' -o -iname '*.webm' \
\) -print0 | while IFS= read -r -d '' source; do
  relative="${source#"$input_dir"/}"
  target="$output_dir/${relative%.*}.mp4"
  mkdir -p "$(dirname "$target")"

  if [[ -e "$target" ]]; then
    echo "skip (already exists): $target"
    continue
  fi

  duration="$(ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "$source")"
  echo "processing: $relative"
  ffmpeg -hide_banner -loglevel warning -nostdin -n \
    -i "$source" \
    -vf "scale=960:720:force_original_aspect_ratio=increase,crop=960:720:(iw-ow)/2:(ih-oh)/2,setsar=1,lenscorrection=k1=-0.08:k2=0.02,vignette=PI/5,drawgrid=w=iw:h=2:t=1:c=black@0.10" \
    -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p \
    -c:a copy -movflags +faststart -progress "$progress_file" "$target" &
  ffmpeg_pid=$!
  last_percent=-1
  while kill -0 "$ffmpeg_pid" 2>/dev/null; do
    encoded_us="$(awk -F= '$1 == "out_time_us" { value=$2 } END { print value + 0 }' "$progress_file")"
    percent="$(awk -v encoded="$encoded_us" -v total="$duration" 'BEGIN {
      if (total > 0) { value = encoded / (total * 1000000) * 100 }
      else { value = 0 }
      if (value < 0) value = 0
      if (value > 99) value = 99
      printf "%d", value
    }')"
    if [[ "$percent" -ne "$last_percent" ]]; then
      printf '\rprocessing: %s [%3d%%]' "$relative" "$percent"
      last_percent="$percent"
    fi
    sleep 1
  done
  wait "$ffmpeg_pid"
  printf '\rprocessing: %s [100%%]\n' "$relative"
done

echo "Done. Originals were not changed."