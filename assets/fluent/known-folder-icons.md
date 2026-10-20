# Known-folder Fluent artwork

The Documents, Pictures and Videos resources are copied without path edits from
Microsoft's Fluent UI System Icons, commit
[`83b26348c811f072429427ff48362238656524fe`](https://github.com/microsoft/fluentui-system-icons/tree/83b26348c811f072429427ff48362238656524fe).

| Local resource | Upstream path within that commit |
| --- | --- |
| [document_24_color.svg](<document_24_color.svg>) | `assets/Document/SVG/ic_fluent_document_24_color.svg` |
| [image_24_color.svg](<image_24_color.svg>) | `assets/Image/SVG/ic_fluent_image_24_color.svg` |
| [video_24_color.svg](<video_24_color.svg>) | `assets/Video/SVG/ic_fluent_video_24_color.svg` |
| [music_note_2_24_color.svg](<music_note_2_24_color.svg>) | `assets/Music Note 2/SVG/ic_fluent_music_note_2_24_filled.svg` |

Music has no upstream 24-pixel color variant at this revision. Its official
filled contour is preserved, with the monochrome fill replaced by a warm
Fluent-style orange gradient. It is an adaptation, not an official color asset.

All four use the existing embedded SVG loader and the existing monochrome
command-icon fallback in high contrast. See the [Microsoft MIT license](<LICENSE>).
