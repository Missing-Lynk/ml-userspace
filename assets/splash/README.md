# Boot splash

`splash.yuv` is the slot-B boot splash: raw I420 1920x1080 (3110400 B), painted on the DRM primary by `ml-splash` (staged as `/usr/local/share/nosignal.yuv` by `rootfs/build.sh` and into the gst squashfs by `gstreamer/scripts/build-prefix.sh`).

Regenerate from `splash.png` (any 16:9 image; non-16:9 needs a crop to 16:9 first):

```sh
ffmpeg -i splash.png -vf "scale=1920:1080:flags=lanczos" -pix_fmt yuv420p -f rawvideo splash.yuv
```
