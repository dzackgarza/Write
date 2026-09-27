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
  Element* stroke = builder->finish();
  document.history->startAction(0);
  page->addStroke(stroke);
  document.history->endAction();
  if(page->strokeCount() != 1 || !document.history->canUndo()) return 1;
  document.history->undo();
  if(page->strokeCount() != 0) return 2;
  document.history->redo();
  if(page->strokeCount() != 1) return 3;

  MemStream svg;
  if(!page->saveSVG(svg)) return 4;
  std::string text(svg.buffer, svg.buffsize);
  return text.find("<path") == std::string::npos ? 5 : 0;
}
