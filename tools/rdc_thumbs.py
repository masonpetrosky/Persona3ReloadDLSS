"""Saves the thumbnail RenderDoc stores in each capture (no replay needed).

Run inside RenderDoc's embedded Python (tools/rdc.sh tools/rdc_thumbs.py) with:
  P3R_RDCS  captures, separated by ';'
  P3R_OUT   output directory (thumbnails are written as <capture name>.png)
"""
import os
import traceback

import renderdoc as rd

OUT = os.environ["P3R_OUT"]
os.makedirs(OUT, exist_ok=True)
LOG = open(os.path.join(OUT, "thumbs.log"), "w", encoding="utf-8")
try:
    for path in os.environ["P3R_RDCS"].split(";"):
        cap = rd.OpenCaptureFile()
        result = cap.OpenFile(path, "", None)
        if result != rd.ResultCode.Succeeded:
            LOG.write("%s: open failed %s\n" % (path, result))
            continue
        thumb = cap.GetThumbnail(rd.FileType.PNG, 0)
        name = os.path.splitext(os.path.basename(path))[0] + ".png"
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(bytes(thumb.data))
        LOG.write("%s: %dx%d\n" % (name, thumb.width, thumb.height))
        cap.Shutdown()
except Exception:
    LOG.write("FATAL " + traceback.format_exc())
finally:
    LOG.close()
    os._exit(0)
