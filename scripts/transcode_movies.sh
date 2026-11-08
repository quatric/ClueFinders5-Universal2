#!/bin/zsh
# usage: transcode_movies.sh <Movies folder>
# Converts the game's Sorenson Video 3 movies (and any H.264 with B-frames) in
# place to H.264 without B-frames, which the runner decodes with VideoToolbox.
# Audio and other tracks are copied unchanged. Animation ('rle ') movies are
# left as they are.
set -e
dir=${1:?usage: transcode_movies.sh <Movies folder>}
for f in "$dir"/*.mov(N); do
	info=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,has_b_frames -of csv=p=0 "$f")
	case $info in
	svq3*|h264,[1-9]*) ;;
	*) continue ;;
	esac
	tmp="${f:r}.tmp.mov"
	ffmpeg -nostdin -v error -y -i "$f" -map 0 -c copy -c:v libx264 -bf 0 -crf 18 -preset slow \
		-pix_fmt yuv420p -tag:v avc1 "$tmp"
	mv "$tmp" "$f"
	echo "transcoded ${f:t} ($info)"
done
