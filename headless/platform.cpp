// The desktop application.cpp defines these header implementations. The
// headless document library uses the same upstream implementations without SDL.
#define PLATFORMUTIL_IMPLEMENTATION
#include "ulib/platformutil.h"
#define STRINGUTIL_IMPLEMENTATION
#include "ulib/stringutil.h"
#define FILEUTIL_IMPLEMENTATION
#include "ulib/fileutil.h"
#define MINIZ_GZ_IMPLEMENTATION
#include "ulib/miniz_gzip.h"
#define FONTSTASH_IMPLEMENTATION
#include "fontstash.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "usvg/svgpainter.h"

void initializeHeadlessWrite()
{
  // Upstream application.cpp installs this bounds calculator before editing.
  static Painter painter(Painter::PAINT_NULL);
  static SvgPainter calculator(&painter);
  SvgDocument::sharedBoundsCalc = &calculator;
}
