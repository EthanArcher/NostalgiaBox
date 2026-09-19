#!/usr/bin/env bash
# Bake the NostalgiaBox CRT look into video copies.
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 INPUT_DIR OUTPUT_DIR [--force]" >&2
  exit 2
fi

force=0
if [[ $# -eq 3 ]]; then
  [[ "$3" == "--force" ]] || { echo "error: unknown option: $3" >&2; exit 2; }
  force=1
fi

input_dir="${1%/}"
output_dir="${2%/}"

# Keep these values in sync with CrtConfig in nostalgiabox/config.py. FFmpeg
# cannot run the mpv GLSL shader, so the lenscorrection filter is the closest
# available equivalent for its barrel warp. Native filters handle the vignette
# and scanlines much faster than evaluating them in geq for every colour plane.
video_filter="lenscorrection=k1=-0.08:k2=0.02,vignette=PI/5,drawgrid=w=iw:h=2:t=1:c=black@0.10,format=rgb24"
corner_mask="if(lte(pow(abs(X/W-0.5)/0.5,6)+pow(abs(Y/H-0.5)/0.5,6),0.94),1,if(gte(pow(abs(X/W-0.5)/0.5,6)+pow(abs(Y/H-0.5)/0.5,6),1.06),0,(1.06-(pow(abs(X/W-0.5)/0.5,6)+pow(abs(Y/H-0.5)/0.5,6)))/0.12))"

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

mask_dir="$(mktemp -d)"
progress_file="$(mktemp)"
mask_file="$mask_dir/corners.png"
trap 'rm -f "$progress_file"; rm -rf "$mask_dir"' EXIT

ffmpeg -hide_banner -loglevel warning -nostdin -y \
  -f lavfi -i "color=white:s=960x720" \
  -vf "format=gray,geq=lum='255*$corner_mask'" \
  -frames:v 1 -pix_fmt gray -update 1 "$mask_file"

find "$input_dir" -type f ! -name '._*' \( \
  -iname '*.mp4' -o -iname '*.mkv' -o -iname '*.avi' -o \
  -iname '*.m4v' -o -iname '*.mov' -o -iname '*.webm' \
\) -print0 | while IFS= read -r -d '' source; do
  relative="${source#"$input_dir"/}"
  target="$output_dir/${relative%.*}.mp4"
  mkdir -p "$(dirname "$target")"

  if [[ -e "$target" && "$force" -eq 0 ]]; then
    echo "skip (already exists): $target"
    continue
  fi

  duration="$(ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "$source")"
  echo "processing: $relative"
  overwrite="-n"
  [[ "$force" -eq 1 ]] && overwrite="-y"
  ffmpeg -hide_banner -loglevel warning -nostdin "$overwrite" \
    -i "$source" \
    -filter_complex "[0:v]scale=960:720:force_original_aspect_ratio=increase,crop=960:720:(iw-ow)/2:(ih-oh)/2,setsar=1,$video_filter,format=gbrp[video];movie=$mask_file,format=gray[mask];color=c=black:s=960x720:r=60:d=$duration,format=gbrp[black];[black][video][mask]maskedmerge,format=yuv420p[out]" \
    -map "[out]" -map 0:a? \
    -c:v libx264 -preset superfast -crf 23 -maxrate 1500k -bufsize 3000k \
    -profile:v high -level:v 4.0 -g 50 -bf 2 -pix_fmt yuv420p \
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