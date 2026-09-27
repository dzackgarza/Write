#include <memory>
#include <string>

#include "syncscribble/document.h"
#include "syncscribble/strokebuilder.h"
#include "headless/platform.h"

int main()
{
  initializeHeadlessWrite();
  Document document;
  auto* page = new Page(PageProperties(595, 842));
  document.insertPage(page);

  std::unique_ptr<StrokeBuilder> builder(StrokeBuilder::create(ScribblePen(Color::BLACK, 3)));
  builder->addInputPoint(StrokePoint(10, 20, 1, 0, 0, 1));
  builder->addInputPoint(StrokePoint(30, 40, 1, 0, 0, 2));
  page->addStroke(builder->finish());

  MemStream svg;
  if(!page->saveSVG(svg)) return 1;
  std::string text(svg.buffer, svg.buffsize);
  return text.find("<path") == std::string::npos ? 2 : 0;
}
