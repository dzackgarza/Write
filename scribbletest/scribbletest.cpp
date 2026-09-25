#include "scribbletest.h"
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <cstdarg>

#include "usvg/svgparser.h"
#include "application.h"
#include "strokebuilder.h"
#include "scribblesync.h"
#include "scribbleapp.h"  // only for sync tests

// Ideally, these tests should be run under valgrind to help check for memory leaks
// renaming out files to refs (Linux):  for i in {0..13}; do mv "test${i}_out.html" "test${i}_ref.html"; done;

// runAll run times (tests 0 - 10):
//  Debian-NDK VM (Qt 4.7 gcc): 440 ms (debug build), 12600 ms (valgrind, 2nd run)
//  Dev-XP VM (Qt 4.8.4, MSVC 2010): 1300 ms (debug), 453 ms (release, standalone)
//  X61T: TBD
// standalone means not started from Qt Creator (so no qDebug output)

// Known sync test issues: 9 (randomStr for bookmark id), 12 (numerical), 14 (numerical)
bool ScribbleTest::exitAfterTest = false;


#ifndef SCRIBBLE_TEST_PATH
#define SCRIBBLE_TEST_PATH "."
#endif

ScribbleTest::ScribbleTest(const std::string& path)
{
  scribbleConfig = new ScribbleConfig();
  scribbleConfig->set("singleFile", true);
  scribbleConfig->set("reduceFileSize", 0);
  // don't use panBorder
  scribbleConfig->set("panFromEdge", false);
  scribbleConfig->set("popupToolbar", false);
  // disable input smoothing
  scribbleConfig->set("inputSmoothing", 0);
  // include thumbnail in test output as a check of rendering
  //  but don't draw page num since fonts are different on different platforms
  scribbleConfig->set("saveThumbnail", 2);
  // don't rely on default page setup
  scribbleConfig->set("pageWidth", 768.0);
  scribbleConfig->set("pageHeight", 1024.0);
  scribbleConfig->set("xRuling", 0.0);
  scribbleConfig->set("yRuling", 40.0);
  scribbleConfig->set("marginLeft", 100.0);
  scribbleConfig->set("ruleColor", Color(Color::BLUE).argb());
  //scribbleConfig->migrateConfig();  // convert xRuling, yRuling, marginLeft to RuleLayer
  // disable DPI detection
  scribbleConfig->set("screenDPI", 150);  //int(ScribbleView::DEFAULT_DPI));
  // disable pen detection (so that mouse input doesn't get disabled)
  scribbleConfig->set("penType", ScribbleInput::DETECTED_PEN);
  //Painter::initPaintSystem();
  scribbleMode = new ScribbleMode(scribbleConfig);
  // NOTE: if you remove or change this, you must find another way to init erase, select, ins space modes!
  scribbleMode->setRuled(true);
  setMode(MODE_STROKE);
  scribbleDoc = new ScribbleDoc(ScribbleApp::app, scribbleConfig, scribbleMode);
  scribbleArea = new ScribbleArea();
  scribbleDoc->addArea(scribbleArea);
  bookmarkArea = new BookmarkView(scribbleConfig, scribbleDoc);
  //bookmarkArea->setScribbleDoc(scribbleDoc);
  syncSlave = NULL;
  waitForSyncDone = false;
  outPath = path;
  // setup ScribbleArea
  scribbleDoc->newDocument();
  screenRect = Rect::ltwh(0,0,600,800);
  screenImg = new Image(screenRect.width(), screenRect.height());
  screenPaint = new Painter(Painter::PAINT_SW, screenImg);  //| Painter::SRGB_AWARE
  setScreenRect(screenRect);
  screenPaint->beginFrame();
  scribbleArea->doPaintEvent(screenPaint);  // ensure that ScribbleView::imgPaint is inited
  screenPaint->endFrame();
  //scribbleArea->setGeometry(Painter::rectToQRect(screenRect));
  swbXML.append_child("swb");
  swbXML.first_child().append_attribute("token").set_value("SCRIBBLE_SYNC_TEST");
  swbXML.first_child().append_attribute("name");
}

ScribbleTest::ScribbleTest(ScribbleDoc* sd, BookmarkView* bv, ScribbleMode* sm)
{
  scribbleDoc = sd;
  scribbleArea = sd->activeArea;
  scribbleMode = sd->scribbleMode;
  bookmarkArea = bv;
  scribbleConfig = NULL;
  screenImg = NULL;
  screenPaint = NULL;
  syncSlave = NULL;
}

ScribbleTest::~ScribbleTest()
{
  if(syncSlave) {
    // ensure disconnect SDL event has been processed before deleting scribbleDoc!
    SDL_Delay(20);
    ScribbleApp::processEvents();
    delete syncSlave;
  }
  if(scribbleConfig) {
    delete screenPaint;
    delete screenImg;
    delete bookmarkArea;
    delete scribbleDoc;
    delete scribbleArea;
    delete scribbleMode;
    delete scribbleConfig;
  }
}

static void distributeTransform(Document* doc)
{
  for(Page* page : doc->pages) {
    for(SvgNode* node : page->contentNode->children()) {
      if(node->type() == SvgNode::PATH && node->hasTransform()) {
        SvgPath* path = static_cast<SvgPath*>(node);
        path->m_path.transform(path->getTransform());
        float w = path->getFloatAttr("stroke-width");
        if(!std::isnan(w))
          path->setAttr<float>("stroke-width", w*path->getTransform().avgScale());
        path->setTransform(Transform2D());
      }
    }
  }
}

void ScribbleTest::startSyncTest(int testnum)
{
  syncTestNum = testnum;
  scribbleDoc->newDocument();
  swbXML.first_child().attribute("name").set_value(fstring("testdoc%d", testnum).c_str());
  scribbleDoc->openSharedDoc(scribbleConfig->String("syncServer"), swbXML.first_child(), false);
  scribbleDoc->scribbleSync->userMessage = [this](std::string msg, int level){ syncSlaveMsg(msg, level); };
  waitForSyncDone = true;
}

void ScribbleTest::checkStrokeMap(ScribbleDoc* doc, const char* msg)
{
  int n = 0;
  for(unsigned int pp = 0; pp < doc->document->pages.size(); pp++)
    n += doc->document->pages[pp]->strokeCount();
  int m = doc->scribbleSync->strokemap.size();
  if(n != m)
    SCRIBBLE_LOG("%s - strokemap count incorrect: %d strokes in document, %d strokes in strokemap", msg, n, m);
}

// wait on disconnect message to save and check slave document and advance to next test
// note that the slave doesn't load any documents
void ScribbleTest::syncSlaveMsg(std::string msg, int level)
{
  if(!StringRef(msg).contains("testuser1 disconnected"))
    return;

  // check strokemap count
  checkStrokeMap(scribbleDoc, "slave (testuser2)");
  distributeTransform(scribbleDoc->document);
  // write output file
  std::string basefile = fstring("%s/test%d", outPath.c_str(), syncTestNum);
  std::string outfile = basefile + "_out.html";
  scribbleDoc->saveDocument(outfile.c_str());
  // compare output to reference; tests of one-file-per-page must handle the svg files themselves
  if(testCompareFiles(outfile.c_str(), (basefile + "_ref.html").c_str(), true))
    scribbleDoc->document->deleteFiles();  //remove(outfile.c_str());
  else
    ++nFailed;
  waitForSyncDone = false;
}

// if svgonly is true, comparison will start from first "<svg" instead of beginning of file
bool ScribbleTest::testCompareFiles(const char* f1, const char* f2, bool svgonly)
{
  std::vector<char> b1;
  std::vector<char> b2;
  readFile(&b1, f1);
  readFile(&b2, f2);
  // fail if either file is missing
  if(b1.empty() || b2.empty())
    return false;
  // convert to null-terminated strings (overwrites last char)
  b1.back() = '\0';
  b2.back() = '\0';
  if(svgonly) {
    char* s1 = strstr(b1.data(), "<svg");
    char* s2 = strstr(b2.data(), "<svg");
    return s1 && s2 && strcmp(s1, s2) == 0;
  }
  return b1.size() == b2.size() && strcmp(b1.data(), b2.data()) == 0;
}

// per-test document setup, shared by runAll and replay
void ScribbleTest::setupTest(const std::string& inpath)
{
  scribbleDoc->newDocument();
  // attempt to open input file; fails quietly if not present
  Document::loadresult_t res = scribbleDoc->openDocument(inpath.c_str());
  if(res == Document::LOAD_NONFATAL) {
    scribbleDoc->checkAndClearErrors();
    scribbleDoc->cfg->set("test_loadErrors", true);
  }
  // reset mode
  scribbleMode->setRuled(true);
  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1, ScribblePen::TIP_ROUND));
  scribbleDoc->app->bookmarkColor = Color::BLUE;
  // prevent unintended updates while inspecting:
  scribbleDoc->cfg->set("savePrompt", true);
}

// write the document in the form runAll compares with a *_ref.html file; shared by runAll and replay
void ScribbleTest::saveTestOutput(const std::string& outfile)
{
  // distribute transform to stroke points to match old behavior ... remove this later(?)
  distributeTransform(scribbleDoc->document);
  screenPaint->beginFrame();
  scribbleArea->doPaintEvent(screenPaint);
  screenPaint->endFrame();
  // include dirtyCount in output file as a check on undo system
  int dirtycount = scribbleDoc->document->dirtyCount;
  for(unsigned int pp = 0; pp < scribbleDoc->document->pages.size(); pp++)
    dirtycount += scribbleDoc->document->pages[pp]->dirtyCount;
  scribbleDoc->cfg->set("test_dirtyCount", dirtycount);
  if(!scribbleDoc->saveDocument(outfile.c_str()))
    SCRIBBLE_LOG("ScribbleTest: error saving %s", outfile.c_str());
}

// a bunch of integration tests ... any "*_out.html" files present after test indicate a failure
// TODO: add some tests to capture scribbleArea->imgPaint->image and compare to a ref

void ScribbleTest::runAll(bool runsynctest)
{
  nFailed = 0;
  int nThumbsFailed = 0;
  std::vector<std::string> slFailed;
  void (ScribbleTest::*tests[])() = {
    &ScribbleTest::test0,
    &ScribbleTest::test1,
    &ScribbleTest::test2,
    &ScribbleTest::test3,
    &ScribbleTest::test4,
    &ScribbleTest::test5,
    &ScribbleTest::test6,
    &ScribbleTest::test7,
    &ScribbleTest::test8,
    &ScribbleTest::test9,
    &ScribbleTest::test10,
    &ScribbleTest::test11,
    &ScribbleTest::test12,
    &ScribbleTest::test13,
    &ScribbleTest::test14,
    &ScribbleTest::test15,
    &ScribbleTest::synctest01
  };
  unsigned int totaltests = sizeof(tests)/sizeof(tests[0]);
  unsigned int nonsynctests = totaltests - 1;  // last test is for sync only

  // create sync slave for sync test
  if(runsynctest) {
    // we are testuser1; slave is testuser2
    scribbleConfig->set("syncUser", "testuser1");
    syncSlave = new ScribbleTest(outPath);
    syncSlave->scribbleConfig->set("syncUser", "testuser2");
    syncSlave->scribbleConfig->set("syncServer", scribbleConfig->String("syncServer"));
    syncSlave->nFailed = 0;
  }

  Dim unitsPerPx = ScribbleView::unitsPerPx;
  ScribbleView::unitsPerPx = 1;
  // these are set in ScribbleApp::loadConfig() ... would be better to pass our scribbleConfig to it
  //RectSelector::HANDLE_SIZE = 4;
  SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = 1;
  Page::BLANK_Y_RULING = scribbleConfig->Float("blankYRuling");  // don't serialize timestamp

  Element::SVG_NO_TIMESTAMP = true;
  SvgWriter::DEFAULT_PATH_DATA_REL = false;
  srandpp(1);  // randomStr now used for bookmark ids
  Timestamp runAllTime = mSecSinceEpoch();
  for(unsigned int ii = 0; true; ii++) {
    std::string basefile = fstring("%s/test%d", outPath.c_str(), ii);
    doCommand(ID_RESETZOOM);
    // enable this to update ref files (saving as *_new.html)
#ifdef REFRESH_REFS
    if(scribbleDoc->openDocument((basefile + "_ref.html").c_str()) == Document::LOAD_OK)
      scribbleDoc->saveDocument((basefile + "_new.html").c_str());
#endif

    setupTest(basefile + "_in.html");

    if(syncSlave) {
      // wait for slave to finish previous test
      while(syncSlave->waitForSyncDone)
        ScribbleApp::processEvents();
      if(ii >= totaltests) {
        syncSlave->scribbleDoc->newDocument();  // disconnect sync
        break;
      }
      // create whiteboard
      //SCRIBBLE_LOG("\nRUNNING TEST %d\n\n", ii);
      swbXML.first_child().attribute("name").set_value(fstring("testdoc%d", ii).c_str());
      scribbleDoc->openSharedDoc(scribbleConfig->String("syncServer"), swbXML.first_child(), true);
      // wait until we connect
      while(!scribbleDoc->scribbleSync->isSyncActive())
        ScribbleApp::processEvents();
      // connect the slave
      syncSlave->startSyncTest(ii);
      // wait until slave connects
      while(!syncSlave->scribbleDoc->scribbleSync->isSyncActive())
        ScribbleApp::processEvents();
    }
    else if(ii >= nonsynctests)
      break;

    // WRITE_RECORD_DIR: write each test's actions to <dir>/test<N>.trace in the replay grammar
    const char* recdir = getenv("WRITE_RECORD_DIR");
    if(recdir && !syncSlave)
      startRecording(fstring("%s/test%d.trace", recdir, ii));
    // array of pointers to member functions, wow...
    (this->*tests[ii])();
    stopRecording();

    // when running sync test, slave writes output
    if(syncSlave) {
      // check strokemap count
      checkStrokeMap(scribbleDoc, "master (testuser1)");
      continue;
    }

    // write output file
    std::string outfile = basefile + "_out.html";
    saveTestOutput(outfile);
    // compare output to reference; tests of one-file-per-page must handle the svg files themselves
    // we've had so many problems with thumbnails that we will count mismatches of those separately
    if(testCompareFiles(outfile.c_str(), (basefile + "_ref.html").c_str()))
      scribbleDoc->document->deleteFiles();  // remove(outfile.c_str());
    else if(testCompareFiles(outfile.c_str(), (basefile + "_ref.html").c_str(), true)) {
      Image refthumb = ScribbleDoc::extractThumbnail((basefile + "_ref.html").c_str());
      Image outthumb = ScribbleDoc::extractThumbnail(outfile.c_str());
      if(outthumb != refthumb) {  //Application::painter->sRGB() &&  && Application::glRender
        nThumbsFailed++;
        std::ofstream refstrm((basefile + "_ref.png").c_str(), std::ios::binary);
        auto refenc = refthumb.encodePNG();
        refstrm.write((char*)refenc.data(), refenc.size());
        std::ofstream outstrm((basefile + "_out.png").c_str(), std::ios::binary);
        auto outenc = outthumb.encodePNG();
        outstrm.write((char*)outenc.data(), outenc.size());
        std::ofstream diffstrm((basefile + "_diff.png").c_str(), std::ios::binary);
        auto diffenc = outthumb.subtract(refthumb, 10, 0).encodePNG();
        diffstrm.write((char*)diffenc.data(), diffenc.size());
      }
    }
    else {
      slFailed.push_back(std::to_string(ii));
      nFailed++;
    }
  }
  runAllTime = mSecSinceEpoch() - runAllTime;
  // restore global config
  srandpp(mSecSinceEpoch());
  SvgWriter::DEFAULT_PATH_DATA_REL = true;
  Element::SVG_NO_TIMESTAMP = false;
  ScribbleView::unitsPerPx = unitsPerPx;
  ScribbleApp::app->loadConfig();
  if(syncSlave)
    nFailed = syncSlave->nFailed;
  resultStr = fstring("Tests completed in %d ms with %d failed tests (%s) and %d failed thumbnails.",
      int(runAllTime), nFailed, joinStr(slFailed, ", ").c_str(), nThumbsFailed);
  //if(!Application::painter->sRGB() || !Application::glRender)
  //  resultStr += "\nWARNING: ScribbleTest requires GL render and sRGB=1 to get correct thumbnails!";
  if(exitAfterTest) {
    SCRIBBLE_LOG(resultStr.c_str());
    exit(nFailed * 0x100 + nThumbsFailed);
  }
}

void ScribbleTest::performanceTest()
{
  gotoPos(0, Point(0,0));
  setMode(MODE_STROKE);
  setPen(ScribblePen(Color::BLACK, 1, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 1.0, 2.0));
  scribbleArea->frameCount = 0;
  Timestamp t0 = mSecSinceEpoch();

  // calibration - allocate 64 MB to ensure buffer doesn't fit in cache
  const int bufferSize = 8192*8192;
  Dim* calBuffer = new Dim[8192*8192];
  for(int ii = 0; ii < bufferSize; ++ii)
    calBuffer[ii] = Dim(ii*4 + 5);
  for(int ii = 0; ii < bufferSize; ++ii)
    calBuffer[ii] = sqrt(calBuffer[bufferSize-ii-1]*2.3 + 3.4);
  delete[] calBuffer;

  Timestamp t1 = mSecSinceEpoch();
  int calibrationTime = t1 - t0;
  t0 = t1;

  // draw many strokes
  for(Dim y = 20; y < 800; y += 20) {
    for(Dim x = 20; x < 800; x += 20) {
      ScribbleApp::processEvents();
      s3(x, y);
      scribbleArea->doRefresh();
    }
  }
  t1 = mSecSinceEpoch();
  int drawTime = t1 - t0;
  t0 = t1;
  int drawFrames = scribbleArea->frameCount;
  scribbleArea->frameCount = 0;
  // select them all
  doCommand(ID_SELALL);
  // move them around
  for(int ii = 0; ii < 10; ii++) {
    ScribbleApp::processEvents();
    s3(400, 400);
    scribbleArea->doRefresh();
  }
  t1 = mSecSinceEpoch();
  int moveTime = t1 - t0;
  t0 = t1;
  int moveFrames = scribbleArea->frameCount;
  scribbleArea->frameCount = 0;
  // scroll up and down a bunch
  setMode(MODE_PAN);
  ie(400, 800, 0, pen, press);
  for(int ii = 0; ii < 2000; ii++) {
    // cleverly generate a triangle wave
    ScribbleApp::processEvents();
    ie(400, 16*ABS((ii % 100) - 50), 0, pen);
    scribbleArea->doRefresh();
  }
  ScribbleApp::processEvents();
  ie(0, 0, 1, pen, release);
  scribbleArea->doRefresh();
  ScribbleApp::processEvents();
  t1 = mSecSinceEpoch();
  int panTime = t1 - t0;
  t0 = t1;
  int panFrames = scribbleArea->frameCount;

  // save, then load file
  std::string outfile = std::string(SCRIBBLE_TEST_PATH) + "/perftest.html";
  scribbleDoc->cfg->set("singleFile", true);
  scribbleDoc->saveDocument(outfile.c_str());
  t1 = mSecSinceEpoch();
  int saveTime = t1 - t0;
  t0 = t1;
  scribbleDoc->openDocument(outfile.c_str());
  t1 = mSecSinceEpoch();
  int loadTime = t1 - t0;
  t0 = t1;
  int fileSize = getFileSize(outfile);
  scribbleDoc->document->deleteFiles();   //remove(outfile.c_str());

  resultStr = fstring(
      "Calibration: %d ms\nDraw: %d frames in %d ms (%f FPS)\nMove: %d frames in %d ms (%f FPS)\nPan: %d frames in %d ms (%f FPS)"
      "\nSave: %d bytes in %d ms\nLoad: %d ms", calibrationTime,
      drawFrames, drawTime, (drawFrames*1000.0)/drawTime, moveFrames, moveTime, (moveFrames*1000.0)/moveTime,
      panFrames, panTime, (panFrames*1000.0)/panTime, fileSize, saveTime, loadTime);
}

// input test; have to use touch since Windows only provides InjectTouchInput (not pen input)
void ScribbleTest::inputTest()
{
  scribbleArea->scribbleInput->singleTouchMode = INPUTMODE_DRAW;
  bool win8ok = false;
  bool injok = true;
#ifdef Q_OS_WIN_XXX
  if(QSysInfo::windowsVersion() > QSysInfo::WV_WINDOWS7) {
    QPoint origin = scribbleArea->mapToGlobal(QPoint(0, 0));
    int x = origin.x();
    int y = origin.y();
    injok = ScribbleInput::injectTouch(x + 150, y + 150, 0.25, press) && injok;
    injok = ScribbleInput::injectTouch(x + 200, y + 200, 0.75) && injok;
    injok = ScribbleInput::injectTouch(x + 250, y + 150, 0.5) && injok;
    injok = ScribbleInput::injectTouch(x + 250, y + 150, 0, release) && injok;
    win8ok = true;
  }
#endif
  if(!win8ok) {
    ie(150, 150, 0.25, INPUTSOURCE_TOUCH, press);
    ie(200, 200, 0.75, INPUTSOURCE_TOUCH);
    ie(250, 150, 0.5, INPUTSOURCE_TOUCH);
    ie(250, 150, 0, INPUTSOURCE_TOUCH, release);
  }
  ie(150, 150, 0.25, INPUTSOURCE_TOUCH, press);
  ie(200, 100, 0.75, INPUTSOURCE_TOUCH);
  ie(250, 150, 0.5, INPUTSOURCE_TOUCH);
  ie(250, 150, 0, INPUTSOURCE_TOUCH, release);
  scribbleArea->scribbleInput->singleTouchMode = INPUTMODE_NONE;
  // this attempted stroke should NOT appear
  ie(150, 200, 0.25, INPUTSOURCE_TOUCH, press);
  ie(200, 150, 0.75, INPUTSOURCE_TOUCH);
  ie(250, 200, 0.5, INPUTSOURCE_TOUCH);
  ie(250, 200, 0, INPUTSOURCE_TOUCH, release);

  resultStr = fstring("A diamond should have been drawn. Windows 8 touch injection OK: %s", (win8ok && injok) ? "true" : "false");
}

// long stroke
void ScribbleTest::s3(Dim xoffset, Dim yoffset)
{
  static const Dim xang[] = {1, 0, -1, 0};
  static const Dim yang[] = {0, -1, 0, 1};
  ie(xoffset, yoffset, 1, pen, press);
  for(int ii = 1; ii <= 100; ii++)
    ie(xoffset + ii*xang[ii % 4]/10.0, yoffset + ii*yang[ii % 4]/10.0, 1, pen);
  ie(0, 0, 1, pen, release);
}

void ScribbleTest::ie(Dim x, Dim y, Dim p, int src, int ev, int mm)
{
  scribbleArea->scribbleInput->doInputEvent(x, y, p, (inputsource_t)src, (inputevent_t)ev, mm, 0);
}

void ScribbleTest::mtinput(inputevent_t ev1, Dim x1, Dim y1, inputevent_t ev2, Dim x2, Dim y2)
{
  InputEvent ievent(INPUTSOURCE_TOUCH, MODEMOD_NONE);
  if(ev1 != INPUTEVENT_NONE)
    ievent.points.push_back(InputPoint(ev1, x1, y1, 1));  // pressure = 1
  if(ev2 != INPUTEVENT_NONE)
    ievent.points.push_back(InputPoint(ev2, x2, y2, 1));
  // touch gestures act on the view, so "mt" keeps screen coordinates; the ie logger must not also see them
  record("mt %d %s %s %d %s %s", int(ev1), ScribbleInput::traceReal(x1).c_str(), ScribbleInput::traceReal(y1).c_str(),
      int(ev2), ScribbleInput::traceReal(x2).c_str(), ScribbleInput::traceReal(y2).c_str());
  FILE* log = ScribbleInput::traceLog;
  ScribbleInput::traceLog = NULL;
  scribbleArea->scribbleInput->doInputEvent(ievent);
  ScribbleInput::traceLog = log;
}

// draw a simple stroke
void ScribbleTest::ss(Dim offset)
{
  ie(104.4 + offset, 184.4 + offset, 0, pen, press);
  ie(124.5 + offset, 162.2 + offset, 0, pen);
  ie(131.8 + offset, 167.3 + offset, 0, pen);
  ie(0, 0, 0, pen, release);
}

void ScribbleTest::s1(Dim xoffset, Dim yoffset)
{
  ie(104.4 + xoffset, 184.4 + yoffset, 0, pen, press);
  ie(124.5 + xoffset, 162.2 + yoffset, 0, pen);
  ie(131.8 + xoffset, 167.3 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);
}

// stroke centered on (0,0)
void ScribbleTest::s2(Dim xoffset, Dim yoffset)
{
  ie(10 + xoffset, -14.8 + yoffset, 0, pen, press);
  ie(-9.9 + xoffset, 0.1 + yoffset, 0, pen);
  ie(9.8 + xoffset, 15.1 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);
}

// Filled stroke similar to s2
void ScribbleTest::f2(Dim xoffset, Dim yoffset)
{
  ScribblePen oldpen = *scribbleDoc->app->getPen();
  setPen(ScribblePen(Color::BLACK, 1.2, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 1.0, 2.0));
  ie(10 + xoffset, -14.8 + yoffset, 0.3, pen, press);
  ie(-9.9 + xoffset, 0.1 + yoffset, 0.7, pen);
  ie(9.8 + xoffset, 15.1 + yoffset, 0.5, pen);
  ie(0, 0, 0, pen, release);
  setPen(oldpen);
}

// MultiStroke (HyperRef) consisting of two strokes
void ScribbleTest::hr(Dim xoffset, Dim yoffset)
{
  ie(-10.1 + xoffset, -14.8 + yoffset, 0, pen, press);
  ie(9.8 + xoffset, 0.1 + yoffset, 0, pen);
  ie(-9.7 + xoffset, 15.1 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);

  ie(-10 + 10 + xoffset, -13.8 + yoffset, 0, pen, press);
  ie(9.9 + 10 + xoffset, 0.1 + yoffset, 0, pen);
  ie(-9.8 + 10 + xoffset, 12.4 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);

  setMode(MODE_SELECTRECT);
  ie(-12 + xoffset, -17 + yoffset, 0, pen, press);
  ie(xoffset, yoffset, 0, pen);
  ie(22 + xoffset, 17 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);
  createHyperRef("http://www.styluslabs.com");
  // have to clear the selection ourselves now
  clearSelection();
}

void ScribbleTest::s3()
{
  ie(213.000, 51.000, 0.200, 1, 1, 0);
  ie(211.000, 51.000, 0.250, 1, 0, 0);
  ie(210.000, 51.000, 0.320, 1, 0, 0);
  ie(206.000, 52.000, 0.390, 1, 0, 0);
  ie(201.000, 54.000, 0.420, 1, 0, 0);
  ie(194.000, 57.000, 0.450, 1, 0, 0);
  ie(188.000, 60.000, 0.440, 1, 0, 0);
  ie(182.000, 64.000, 0.370, 1, 0, 0);
  ie(179.000, 66.000, 0.390, 1, 0, 0);
  ie(175.000, 68.000, 0.310, 1, 0, 0);
  ie(174.000, 69.000, 0.280, 1, 0, 0);
  ie(173.000, 70.000, 0.220, 1, 0, 0);
  ie(172.000, 71.000, 0.210, 1, 0, 0);
  ie(172.000, 72.000, 0.170, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(176.000, 46.000, 0.190, 1, 1, 0);
  ie(178.000, 47.000, 0.260, 1, 0, 0);
  ie(181.000, 51.000, 0.330, 1, 0, 0);
  ie(185.000, 56.000, 0.370, 1, 0, 0);
  ie(190.000, 63.000, 0.420, 1, 0, 0);
  ie(194.000, 68.000, 0.390, 1, 0, 0);
  ie(197.000, 72.000, 0.340, 1, 0, 0);
  ie(202.000, 78.000, 0.250, 1, 0, 0);
  ie(203.000, 80.000, 0.280, 1, 0, 0);
  ie(205.000, 82.000, 0.230, 1, 0, 0);
  ie(205.000, 83.000, 0.190, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
}

void ScribbleTest::s4()
{
  ie(217.000, 46.000, 0.200, 1, 1, 0);
  ie(215.000, 46.000, 0.250, 1, 0, 0);
  ie(212.000, 48.000, 0.320, 1, 0, 0);
  ie(209.000, 51.000, 0.390, 1, 0, 0);
  ie(203.000, 55.000, 0.420, 1, 0, 0);
  ie(198.000, 58.000, 0.450, 1, 0, 0);
  ie(192.000, 62.000, 0.440, 1, 0, 0);
  ie(190.000, 63.000, 0.370, 1, 0, 0);
  ie(187.000, 66.000, 0.390, 1, 0, 0);
  ie(184.000, 68.000, 0.310, 1, 0, 0);
  ie(181.000, 71.000, 0.280, 1, 0, 0);
  ie(178.000, 72.000, 0.220, 1, 0, 0);
  ie(174.000, 75.000, 0.210, 1, 0, 0);
  ie(172.000, 76.000, 0.170, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(179.000, 54.000, 0.200, 1, 1, 0);
  ie(180.000, 54.000, 0.250, 1, 0, 0);
  ie(181.000, 54.000, 0.320, 1, 0, 0);
  ie(182.000, 55.000, 0.390, 1, 0, 0);
  ie(184.000, 58.000, 0.420, 1, 0, 0);
  ie(187.000, 62.000, 0.450, 1, 0, 0);
  ie(192.000, 68.000, 0.440, 1, 0, 0);
  ie(194.000, 71.000, 0.370, 1, 0, 0);
  ie(198.000, 75.000, 0.390, 1, 0, 0);
  ie(201.000, 80.000, 0.310, 1, 0, 0);
  ie(205.000, 82.000, 0.280, 1, 0, 0);
  ie(208.000, 84.000, 0.220, 1, 0, 0);
  ie(210.000, 85.000, 0.210, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
}

void ScribbleTest::test0()
{
  //gotoPos(0, Point(0,0));
  ie(104.4, 184.4, 0, pen, press);
  ie(124.5, 162.2, 0, pen);
  ie(131.8, 167.3, 0, pen);
  ie(0, 0, 0, pen, release);

  setMode(MODE_INSSPACERULED);
  ie(14.4, 56.4, 0, pen, press);
  ie(16.5, 85.2, 0, pen);
  ie(16.5, 99.9, 0, pen);
  ie(15.8, 112.3, 0, pen);
  ie(0, 0, 0, pen, release);

  // pinch zoom (will also test rounding to nearest zoom level)
  mtinput(INPUTEVENT_PRESS, 120, 140, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 135, 128, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 132, 125, INPUTEVENT_PRESS, 345, 327);
  mtinput(INPUTEVENT_MOVE, 144, 137, INPUTEVENT_MOVE, 340, 317);
  mtinput(INPUTEVENT_MOVE, 200, 197, INPUTEVENT_MOVE, 290, 295);
  mtinput(INPUTEVENT_MOVE, 208, 204, INPUTEVENT_MOVE, 278, 275);  // zoom step >2 or <0.5 now cancels zoom
  mtinput(INPUTEVENT_MOVE, 216, 210, INPUTEVENT_MOVE, 264, 256);
  mtinput(INPUTEVENT_RELEASE, 216, 210, INPUTEVENT_MOVE, 262, 256);
  mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_MOVE, 260, 254);
  mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_MOVE, 260, 246);
  mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_RELEASE, 260, 246);

  // test cancellation of touch
  mtinput(INPUTEVENT_PRESS, 120, 140, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 135, 128, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 132, 125, INPUTEVENT_PRESS, 345, 327);
  mtinput(INPUTEVENT_MOVE, 144, 137, INPUTEVENT_MOVE, 340, 317);
  // this doesn't do anything - rather the pen input from s2(...) call cancels and starts drawing
  //mtinput(INPUTEVENT_CANCEL, 200, 197, INPUTEVENT_CANCEL, 290, 295);

  // draw something else
  s2(301, 402);
}

// similar to test0, but loads file test1_in.html - a damaged (truncated) file to test error robustness
void ScribbleTest::test1()
{
  ie(104.4, 184.4, 0, pen, press);
  ie(114.5, 172.2, 0, pen);
  ie(121.8, 177.3, 0, pen);
  ie(0, 0, 0, pen, release);

  setMode(MODE_INSSPACERULED);
  ie(14.4, 56.4, 0, pen, press);
  ie(16.5, 86.2, 0, pen);
  ie(15.8, 113.3, 0, pen);
  ie(0, 0, 0, pen, release);
}

// draw an "X", select and move it, draw another "X"
void ScribbleTest::test2()
{
  ie(104.4, 184.4, 0, 1, 1, 0);
  ie(114.5, 172.2, 0, 1, 0, 0);
  ie(121.8, 177.3, 0, 1, 0, 0);
  ie(0, 0, 0, 1, -1, 0);
  undo();
  s3();
  ie(154.000, 29.000, 0.000, 1, 1, 2);
  ie(154.000, 30.000, 0.000, 1, 0, 0);
  ie(155.000, 30.000, 0.000, 1, 0, 0);
  ie(157.000, 33.000, 0.000, 1, 0, 0);
  ie(160.000, 36.000, 0.000, 1, 0, 0);
  ie(167.000, 46.000, 0.000, 1, 0, 0);
  ie(174.000, 55.000, 0.000, 1, 0, 0);
  ie(188.000, 67.000, 0.000, 1, 0, 0);
  ie(195.000, 76.000, 0.000, 1, 0, 0);
  ie(203.000, 85.000, 0.000, 1, 0, 0);
  ie(212.000, 96.000, 0.000, 1, 0, 0);
  ie(216.000, 102.000, 0.000, 1, 0, 0);
  ie(221.000, 108.000, 0.000, 1, 0, 0);
  ie(224.000, 112.000, 0.000, 1, 0, 0);
  ie(227.000, 115.000, 0.000, 1, 0, 0);
  ie(228.000, 117.000, 0.000, 1, 0, 0);
  ie(229.000, 117.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(193.000, 65.000, 0.000, 1, 1, 0);
  ie(194.000, 65.000, 0.000, 1, 0, 0);
  ie(197.000, 68.000, 0.000, 1, 0, 0);
  ie(204.000, 73.000, 0.000, 1, 0, 0);
  ie(214.000, 81.000, 0.000, 1, 0, 0);
  ie(228.000, 90.000, 0.000, 1, 0, 0);
  ie(261.000, 116.000, 0.000, 1, 0, 0);
  ie(294.000, 142.000, 0.000, 1, 0, 0);
  ie(329.000, 166.000, 0.000, 1, 0, 0);
  ie(387.000, 218.000, 0.000, 1, 0, 0);
  ie(425.000, 257.000, 0.000, 1, 0, 0);
  ie(483.000, 315.000, 0.000, 1, 0, 0);
  ie(509.000, 363.000, 0.000, 1, 0, 0);
  ie(544.000, 438.000, 0.000, 1, 0, 0);
  ie(559.000, 476.000, 0.000, 1, 0, 0);
  ie(569.000, 535.000, 0.000, 1, 0, 0);
  ie(569.000, 562.000, 0.000, 1, 0, 0);
  ie(559.000, 602.000, 0.000, 1, 0, 0);
  ie(549.000, 625.000, 0.000, 1, 0, 0);
  ie(534.000, 649.000, 0.000, 1, 0, 0);
  ie(524.000, 663.000, 0.000, 1, 0, 0);
  ie(509.000, 683.000, 0.000, 1, 0, 0);
  ie(502.000, 693.000, 0.000, 1, 0, 0);
  ie(491.000, 707.000, 0.000, 1, 0, 0);
  ie(486.000, 714.000, 0.000, 1, 0, 0);
  ie(483.000, 719.000, 0.000, 1, 0, 0);
  ie(480.000, 723.000, 0.000, 1, 0, 0);
  ie(479.000, 726.000, 0.000, 1, 0, 0);
  ie(478.000, 728.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(176.000, 70.000, 0.000, 1, 1, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  s4();
}

// this test is mainly intended for finding memory leaks (so run it under valgrind)
// also, this test will now fail if undo does not preserve z-order
void ScribbleTest::test3()
{
  ss(0);
  hr(300, 300);
  ss(5);
  doCommand(ID_SELALL);
  doCommand(ID_DELSEL);
  undo();
  undo();
  ss(10);
  ss(15);
  undo();
  ss(20);
  doCommand(ID_SELALL);
  doCommand(ID_COPYSEL);
  doCommand(ID_DELSEL);
  ss(30);
  ss(40);
  doCommand(ID_SELALL);
  doCommand(ID_COPYSEL);
  doCommand(ID_PASTE);
  ss(50);
  undo();
  undo();
  doCommand(ID_PASTE);
  doCommand(ID_DELSEL);
  ss(60);
  undo();
  undo();
  ss(70);
  doCommand(ID_PASTE);
  doCommand(ID_DELSEL);
  undo();
  undo();
  ss(80);
  // none of the tests actually covered pasted strokes - there was a SWB bug
  doCommand(ID_PASTE);
}

// test deletion of page with active selection
void ScribbleTest::test4()
{
  ss(0);
  doCommand(ID_NEXTPAGENEW);
  ss(10);
  ss(20);
  doCommand(ID_SELALL);
  doCommand(ID_COPYSEL);
  doCommand(ID_DELPAGE);
  undo();
  // delete strokes from page, then undo that - added in response to SWB bug
  doCommand(ID_SELALL);
  doCommand(ID_DELSEL);
  undo();

  doCommand(ID_NEXTPAGE);  // make sure we're on second page
  //doCommand(ID_PASTE);
  setMode(MODE_PAGESEL);
  // tap on page
  ie(300, 400, 0, pen, press);
  ie(301, 399, 0, pen);
  ie(0, 0, 0, pen, release);
  doCommand(ID_CUTSEL);

  doCommand(ID_NEXTSCREEN);  // want to paste after first page
  doCommand(ID_PASTE);
  doCommand(ID_PASTE);
  gotoPage(0);  // restore view
}

// test setting page properties and growing page
void ScribbleTest::test5()
{
  PageProperties props(600, 800, 30, 30, 30, Color::YELLOW, Color(0, 0, 0xFF, 0x7F));
  ss(0);
  ss(10);
  setPageProperties(&props, false, true, false);
  ss(20);
  // add new page
  doCommand(ID_NEXTPAGENEW);
  ss(0);
  // grow new page ... off page strokes can't grow page any more, so delete afterwards!
  ie(104.4, 900, 0, pen, press);
  ie(124.5, 803, 0, pen);
  ie(131.8, 900, 0, pen);
  ie(0, 0, 0, pen, release);
  recentStrokeSelect();
  doCommand(ID_DELSEL);
  ss(20);
}

// test opening file (test6_in.html) to saved position and lasso selection
void ScribbleTest::test6()
{
  // make a lasso selection
  setMode(MODE_SELECTLASSO);
  ie(475.000, 199.000, 0.000, 1, 1, 0);
  ie(450.000, 175.000, 0.000, 1, 0, 0);
  ie(329.000, 269.000, 0.000, 1, 0, 0);
  ie(435.000, 386.000, 0.000, 1, 0, 0);
  ie(553.000, 268.000, 0.000, 1, 0, 0);
  ie(516.000, 240.000, 0.000, 1, 0, 0);
  ie(436.000, 317.000, 0.000, 1, 0, 0);
  ie(387.000, 269.000, 0.000, 1, 0, 0);
  ie(452.000, 215.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  // tap selection to switch to ruled mode
  ie(442, 262, 0, pen, press);
  ie(441, 262, 0, pen);
  ie(0, 0, 0, pen, release);
  // now move the selection
  ie(441.000, 263.000, 0.000, 1, 1, 0);
  ie(432.000, 319.000, 0.000, 1, 0, 0);
  ie(431.000, 326.000, 0.000, 1, 0, 0);
  ie(431.000, 342.000, 0.000, 1, 0, 0);
  ie(429.000, 399.000, 0.000, 1, 0, 0);
  ie(429.000, 407.000, 0.000, 1, 0, 0);
  ie(424.000, 454.000, 0.000, 1, 0, 0);
  ie(425.000, 473.000, 0.000, 1, 0, 0);
  ie(429.000, 476.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);

  // tests added for dup sel (and for resaving page containing jpeg image)
  doCommand(ID_NEXTPAGE);
  // ruled select in left margin
  setMode(MODE_SELECTRULED);
  ie(24, 61, 0, pen, press);
  ie(23, 139, 0, pen);
  ie(0, 0, 0, pen, release);
  doCommand(ID_DUPSEL);
  doCommand(ID_PREVPAGE);
}

// test insert space reflow
void ScribbleTest::test7()
{
  // easier to figure out what's going on if screen space and Dim space are the same
  gotoPos(0, Point(0,0));
  PageProperties props(700, 800, 0, 40, 100);
  setPageProperties(&props, false, true, false);
  // create 3 lines of "text";  note: width of ss is 27.4
  for(int offset = 15; offset < 500; offset += 139) {
    s1(offset, 0);
    s1(offset + 30, 0);
    s1(offset + 60, 0);
    s1(offset + 90, 0);
  }
  for(int offset = 5; offset < 500; offset += 141) {
    s1(offset, 40);
    s1(offset + 30, 40);
    s1(offset + 60, 40);
    s1(offset + 90, 40);
  }
  for(int offset = 10; offset < 500; offset += 140) {
    s1(offset, 80);
    s1(offset + 30, 80);
    s1(offset + 60, 80);
    s1(offset + 90, 80);
  }
  // insert space on first line
  setMode(MODE_INSSPACERULED);
  ie(102, 180, 0, pen, press);
  ie(200, 180, 0, pen);
  ie(310, 180, 0, pen);
  ie(0, 0, 0, pen, release);

  // insert some more space, then undo
  setMode(MODE_INSSPACERULED);
  ie(102, 180, 0, pen, press);
  ie(200, 180, 0, pen);
  ie(310, 180, 0, pen);
  ie(0, 0, 0, pen, release);
  undo();

  // test non-ruled insert space
  s1(0, 280);
  s1(30, 280);
  s1(60, 280);
  s1(90, 280);
  setMode(MODE_INSSPACEVERT);
  ie(102, 420, 0, pen, press);
  ie(103, 440, 0, pen);
  ie(105, 470, 0, pen);
  ie(0, 0, 0, pen, release);
}

// test ruled select and moving strokes between pages
void ScribbleTest::test8()
{
  PageProperties props(700, 800, 0, 40, 100);
  setPageProperties(&props, false, true, false);
  // add new page
  s2(400,60);
  doCommand(ID_NEXTPAGENEW);
  gotoPos(0, Point(0,0));
  doCommand(ID_SELALL);
  doCommand(ID_DELSEL);
  s2(200, 380);
  s2(150, 420);
  s2(200, 420);
  f2(250, 420);
  s2(600, 420);
  s2(200, 460);
  setMode(MODE_SELECTRULED);
  ie(110, 420, 0, pen, press);
  ie(275, 420, 0, pen);
  ie(350, 420, 0, pen);
  ie(0, 0, 0, pen, release);
  // hack to workaround new behavior that dropping selection outside screen area will not move
  setScreenRect(Rect::ltwh(0, 0, 600, 1400));
  // drag selection to second page
  ie(200, 420, 0, pen, press);
  ie(210, 750, 0, pen);
  ie(215, 1300 - 40, 0, pen);  // -40 accounts for change of interpage gap from 60 to 20
  ie(0, 0, 0, pen, release);
  setScreenRect(screenRect);
  // put pen down to clear selection
  s2(500, 1300);

  // return to first page and test erasers
  gotoPos(0, Point(0,0));
  s2(200, 60);
  s2(150, 100);
  s2(200, 100);
  s2(250, 100);
  s2(600, 100);
  s2(200, 140);
  setMode(MODE_ERASERULED);
  ie(110, 100, 0, pen, press);
  ie(150, 100, 0, pen);
  ie(200, 100, 0, pen);
  ie(250, 130, 0, pen);
  ie(150, 130, 0, pen);
  ie(0, 0, 0, pen, release);
  setMode(MODE_ERASESTROKE);
  ie(599, 62, 0, pen, press);
  ie(600, 100, 0, pen);
  ie(602, 139, 0, pen);
  ie(0, 0, 0, pen, release);
}

// test ruled mode tools on unruled page
void ScribbleTest::test9()
{
  gotoPos(0, Point(0,0));
  PageProperties props(700, 800, 0, 0, 0);
  setPageProperties(&props, false, true, false);
  // create 4 lines of "text"
  for(int offset = 125; offset < 600; offset += 125) {
    s2(offset, 41);
    s2(offset + 25, 37);
    s2(offset + 50, 42);
    s2(offset + 75, 40);
  }
  for(int offset = 125; offset < 600; offset += 118) {
    s2(offset, 82);
    s2(offset + 25, 79);
    s2(offset + 50, 77);
    s2(offset + 75, 81);
  }
  for(int offset = 115; offset < 600; offset += 121) {
    s2(offset, 118);
    s2(offset + 25, 117);
    s2(offset + 50, 123);
    s2(offset + 75, 120);
  }
  for(int offset = 120; offset < 600; offset += 120) {
    s2(offset, 160);
    s2(offset + 26, 161);
    s2(offset + 53, 157);
    s2(offset + 76, 159);
  }
  // add a bookmark
  setMode(MODE_BOOKMARK);
  ie(135, 110, 0, pen, press);
  ie(106, 140, 0, pen);
  ie(60, 160, 0, pen);
  ie(0, 0, 0, pen, release);
  // insert space
  setMode(MODE_INSSPACERULED);
  ie(235, 110, 0, pen, press);
  ie(206, 140, 0, pen);
  ie(184, 195, 0, pen);
  ie(0, 0, 0, pen, release);
  // ruled erase
  setMode(MODE_ERASERULED);
  ie(60, 90, 0, pen, press);
  ie(140, 92, 0, pen);
  ie(200, 87, 0, pen);
  ie(290, 86, 0, pen);
  ie(385, 89, 0, pen);
  ie(0, 0, 0, pen, release);

  // drop a bookmark off page, then undo - added because this used to cause crash on undo
  setMode(MODE_BOOKMARK);
  ie(50, 50, 0, pen, press);
  ie(20, 20, 0, pen);
  ie(-100, -100, 0, pen);
  ie(0, 0, 0, pen, release);
  undo();
  redo();

  // must refresh bookmarks so that findBookmark will work
  //bookmarkArea->repaintBookmarks();
  paintBookmarks();

  doCommand(ID_NEXTPAGENEW);
  gotoPos(1, Point(-10,-10));  // account for previous behavior of NEXTPAGE
  // create hyperref, then convert (along with another stroke) to point to bookmark on first page
  hr(150, 100);
  s2(200, 100);
  doCommand(ID_SELALL);
  createHyperRefToBookmark(20);

  // add bookmark to 2nd page ... id should not be serialized for this one
  setMode(MODE_BOOKMARK);
  ie(135, 150, 0, pen, press);
  ie(106, 180, 0, pen);
  ie(65, 200, 0, pen);
  ie(0, 0, 0, pen, release);

  // return to first page and copy the bookmark - the was crashing before we fixed clone bug with idStr
  doCommand(ID_PREVPAGE);
  doCommand(ID_SELALL);
  doCommand(ID_COPYSEL);
  doCommand(ID_PASTE);
  doCommand(ID_DELSEL);
  // make sure *cut* and paste preserves id
  doCommand(ID_SELALL);
  doCommand(ID_CUTSEL);
  doCommand(ID_PASTE);
  doCommand(ID_NEXTPAGENEW);
}

// test changing properties and ins space ruled with hyperref
void ScribbleTest::test10()
{
  // test creating new doc with different view modes (added because of crash)
  if(!syncSlave) {
    cfgSet("viewMode", 0);
    newDocument();
    cfgSet("viewMode", 2);
    newDocument();
    cfgSet("viewMode", 1);
    newDocument();
  }

  gotoPos(0, Point(0,0));
  hr(150, 100);
  s2(200, 100);
  f2(250, 100);
  doCommand(ID_SELALL);

  setStrokeProperties(StrokeProperties(Color::BLUE, -1));
  setStrokeProperties(StrokeProperties(Color::INVALID_COLOR, 2));
  setStrokeProperties(StrokeProperties(Color::INVALID_COLOR, 3));
  undo();

  // added due to bug: failed to create undo items for translated multistrokes
  setMode(MODE_INSSPACERULED);
  ie(104, 100, 0, pen, press);
  ie(195, 98, 0, pen);
  ie(244, 103, 0, pen);
  ie(0, 0, 0, pen, release);
  undo();

  // Note pen not longer changed when selection active
  setMode(MODE_INSSPACERULED);
  ie(104, 100, 0, pen, press);
  ie(195, 98, 0, pen);
  ie(298, 103, 0, pen);
  ie(0, 0, 0, pen, release);
}

// test column detection and free erase
void ScribbleTest::test11()
{
  gotoPos(0, Point(0,0));
  PageProperties props(1200, 800, 0, 40, 100);
  setPageProperties(&props, false, true, false);
  // create 2 columns with 4 lines of "text" each
  for(int col = 0; col < 1000; col += 550) {
    for(int offset = 125; offset < 600; offset += 125) {
      s2(col + offset, 61);
      s2(col + offset + 25, 67);
      s2(col + offset + 50, 62);
      s2(col + offset + 75, 60);
    }
    for(int offset = 130; offset < 600; offset += 119) {
      s2(col + offset, 102);
      s2(col + offset + 25, 99);
      s2(col + offset + 50, 97);
      s2(col + offset + 75, 101);
    }
    for(int offset = 125; offset < 600; offset += 121) {
      s2(col + offset, 138);
      s2(col + offset + 25, 137);
      s2(col + offset + 50, 143);
      s2(col + offset + 75, 140);
    }
    for(int offset = 120; offset < 600; offset += 120) {
      s2(col + offset, 180);
      s2(col + offset + 26, 181);
      s2(col + offset + 53, 177);
      s2(col + offset + 76, 179);
    }
  }
  // need dense points
  //PointDensityFilter::MAX_POINT_DIST = 2;
  // draw column divider - points need to be dense and we've disabled MAX_POINT_DIST
  ie(640, 20, 0, pen, press);
  ie(605, 220, 0, pen);
  ie(0, 0, 0, pen, release);

  // insert space
  setMode(MODE_INSSPACERULED);
  ie(225, 62, 0, pen, press);
  ie(350, 64, 0, pen);
  ie(505, 63, 0, pen);
  ie(0, 0, 0, pen, release);

  // apply free eraser to stroke, filled stroke, hyperref
  // ... also want a stroke which will be broken into > 2 pieces
  ie(140, 420, 0, pen, press);
  ie(145, 340, 0, pen);
  ie(150, 420, 0, pen);
  ie(155, 340, 0, pen);
  ie(160, 420, 0, pen);
  ie(0, 0, 0, pen, release);
  s2(200, 379);
  f2(250, 382);
  hr(300, 381);
  setMode(MODE_ERASEFREE);
  ie(100, 380, 0, pen, press);
  ie(140, 380, 0, pen);
  ie(200, 380, 0, pen);
  ie(290, 380, 0, pen);
  ie(400, 380, 0, pen);
  ie(0, 0, 0, pen, release);
  //PointDensityFilter::MAX_POINT_DIST = 1000;
}

// test scaling ... test12_in.html includes an image
void ScribbleTest::test12()
{
  // easier to figure out what's going on if screen space and Dim space are the same
  gotoPos(0, Point(0,0));
  // test added because of crash
  setMode(MODE_BOOKMARK);
  ie(135, 110, 0, pen, press);
  ie(106, 100, 0, pen);
  ie(60, 60, 0, pen);
  ie(0, 0, 0, pen, release);
  //scribbleDoc->document->drawBookmarks(screenPaint, Rect());
  paintBookmarks();
  undo();
  // back to our regularly scheduled programming...
  s2(200, 179);
  f2(250, 182);
  hr(300, 181);
  doCommand(ID_SELALL);
  // tap the selection to change to move sel free mode (since selectAll no longer ignores moveSelMode) ... nevermind
  //ie(300, 160, 0, pen, press);
  //ie(298, 162, 0, pen);
  //ie(0, 0, 0, pen, release);
  // resize selection
  ie(320, 198, 0, pen, press);
  ie(350, 250, 0, pen);
  ie(450, 300, 0, pen);
  ie(0, 0, 0, pen, release);
  // clear selection
  s2(550, 550);
  doCommand(ID_UNDO);
  doCommand(ID_REDO);
  // apply free eraser to image
  setMode(MODE_ERASEFREE);
  ie(100, 40, 0, pen, press);
  ie(150, 90, 0, pen);
  ie(200, 140, 0, pen);
  ie(0, 0, 0, pen, release);
}

// test13 - test multiple file documents, since these have been the source of the two data loss bugs found so
//  far in Write
void ScribbleTest::test13()
{
  // setup output path
  std::string sfilename = outPath + u8"/test13_\u4E0B\u5348.html";
  const char* filename = sfilename.c_str();

  // create page 1
  gotoPos(0, Point(0,0));
  s2(100, 100);
  // create page 2
  doCommand(ID_NEXTPAGENEW);
  gotoPos(1, Point(-10,-10));  // account for previous behavior of NEXTPAGE
  s2(200, 200);
  // create page 3
  doCommand(ID_NEXTPAGENEW);
  gotoPos(2, Point(-10,-10));  // account for previous behavior of NEXTPAGE
  s2(300, 300);
  // undo and redo page creation ... added because of a crash
  doCommand(ID_UNDO);
  doCommand(ID_UNDO);
  doCommand(ID_REDO);
  doCommand(ID_REDO);
  // return to page 1
  gotoPos(0, Point(0,0));
  // save
  if(!syncSlave) {
    //scribbleArea->cfg->set("singleFile", false);
    reopen(filename, Document::SAVE_MULTIFILE, false);
    // reopen; only page 1 should be loaded
  }

  // change page properties - apply to all
  PageProperties props(0, 0, 30, 30, 30, Color::YELLOW, Color(0, 0, 0xFF, 0x7F));
  setPageProperties(&props, true, false, false);
  // save and reopen
  if(!syncSlave)
    reopen(filename, 0, false);

  // insert a new page after page 1
  doCommand(ID_PAGEAFTER);
  gotoPos(1, Point(-10,-10));  // account for previous behavior of NEXTPAGE
  s2(150, 150);
  // save and reopen
  if(!syncSlave)
    reopen(filename, 0, false);

  // remove page 3 (formerly page 2)
  doCommand(ID_NEXTPAGENEW);
  doCommand(ID_DELPAGE);
  // save and reopen
  if(!syncSlave) {
    // load all pages before deleting SVG files
    reopen(filename, 0, true);
    // test other file types
    reopen(outPath + u8"/test13_\u4E0B\u5348.svg", 0, true);
    reopen(outPath + u8"/test13_\u4E0B\u5348.svgz", 0, true);
  }
}

void ScribbleTest::test14()
{
  setMoveSelMode(MODE_MOVESELFREE);  // selectAll no longer ignores moveSelMode
  // easier to figure out what's going on if screen space and Dim space are the same
  gotoPos(0, Point(0,0));

  s2(200, 179);
  f2(250, 182);
  hr(300, 181);
  doCommand(ID_SELALL);
  // test scale selection with one negative and one positive scale factor
  ie(188, 162, 0, pen, press); // top left corner
  ie(350, 162, 0, pen);
  ie(450, 162, 0, pen);
  ie(0, 0, 0, pen, release);

  // test scale w/ stroke width scaling
  ie(450, 198, 0, pen, press, MODEMOD_PENBTN);
  ie(500, 250, 0, pen, INPUTEVENT_MOVE, MODEMOD_PENBTN);
  ie(550, 300, 0, pen, INPUTEVENT_MOVE, MODEMOD_PENBTN);
  ie(0, 0, 0, pen, release);

  // test rotation
  ie(435, 140, 0, pen, press);
  ie(460, 160, 0, pen);
  ie(490, 190, 0, pen);
  ie(0, 0, 0, pen, release);

  // why not...
  doCommand(ID_UNDO);
  doCommand(ID_REDO);

  // test smoothing and simplification
  setPen(ScribblePen(Color::BLACK, 1.2, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 1.0, 2.0));
  cfgSet("inputSimplify", 4);
  s3();
  cfgSet("inputSmoothing", 4);
  s4();
  cfgSet("inputSmoothing", 0);
  cfgSet("inputSimplify", 0);
}

// add snap to grid and line drawing tests too!
void ScribbleTest::test15()
{
  // switch to relative path data
  setPathDataRel(true);

  gotoPos(0, Point(0,0));
  s2(200, 179);
  f2(250, 182);
  hr(300, 181);

  // make sure realToStr doesn't write "-0"
  ie(400, 180, 0, pen, press);
  ie(410, 180.0004, 0, pen);
  ie(420, 180, 0, pen);
  ie(0, 0, 0, pen, release);

  // test z-order for highlighter (draw under)
  setPen(ScribblePen(Color::YELLOW, 20, ScribblePen::TIP_CHISEL | ScribblePen::DRAW_UNDER));
  s2(250, 190);
  undo();
  redo();

  s2(310, 180);
  setMode(MODE_ERASESTROKE);
  ie(300, 180, 0, pen, press);
  ie(300, 180, 0, pen);
  ie(0, 0, 0, pen, release);
  undo();

  // test of reflow that should fail w/ cmpRuled bug (not sorting strokes on line left-to-right)
  setPen(ScribblePen(Color::BLACK, 1, ScribblePen::TIP_ROUND));
  for(int offset = 15; offset < 500; offset += 139) {
    s1(offset + 60, 120);
    s1(offset + 90, 120);
    s1(offset, 120);
    s1(offset + 30, 120);
  }
  // insert space
  setMode(MODE_INSSPACERULED);
  ie(102, 300, 0, pen, press);
  ie(200, 300, 0, pen);
  ie(310, 300, 0, pen);
  ie(0, 0, 0, pen, release);

  // test erase on negative insert space
  s2(180 + 60, 420);
  s2(180 + 90, 420);
  s2(180,      420);
  s2(180 + 30, 420);

  setMode(MODE_INSSPACERULED);
  ie(180 + 45, 420, 0, pen, press);
  ie(180, 420, 0, pen);
  ie(140, 420, 0, pen);
  ie(0, 0, 0, pen, release);

  setClipboardSvg(
      "<g stroke='green' transform='translate(50, 50)'><rect fill='rgba(0, 0, 255, 0.8)' stroke='none' x='0' y='0' width='20' height='20'/>"
      "<path fill='red' d='M10 10 l2 4 1 2 0 3 -1 1 -1 0z'/></g>");

  doCommand(ID_PASTE);
  doCommand(ID_DELSEL);
  undo();
  undo();
  doCommand(ID_PASTE);
}

// back and forth test for whiteboard

void ScribbleTest::waitForSync()
{
  // wait until all items are sent and echoed back from server, then wait until slave has received all items (same number of byyes)
  // alternative, wait until master and slave stroke counts are equal?
  ScribbleSync* ss = scribbleDoc->scribbleSync;
  while(ss->canSendHist() || ss->rcvdPos != ss->sentPos || ss->bytesRcvd > syncSlave->scribbleDoc->scribbleSync->bytesRcvd)
    ScribbleApp::processEvents();
}

void ScribbleTest::synctest01()
{
  gotoPos(0, Point(0,0));
  hr(150, 100);
  s2(200, 100);
  f2(250, 100);
  doCommand(ID_SELALL);

  waitForSync();
  syncSlave->synctest01slave1();

  // move the selection
  ie(200, 100, 0, pen, press);
  ie(210, 120, 0, pen);
  ie(220, 160, 0, pen);
  ie(0, 0, 0, pen, release);

  waitForSync();
  syncSlave->synctest01slave2();
}

void ScribbleTest::synctest01slave1()
{
  gotoPos(0, Point(0,0));
  s2(550, 550);
  // apply free eraser to image
  setMode(MODE_ERASERULED);
  ie(245, 101, 0, pen, press);
  ie(270, 99, 0, pen);
  ie(300, 100, 0, pen);
  ie(0, 0, 0, pen, release);
}

void ScribbleTest::synctest01slave2()
{
  // restore the deleted stroke
  undo();

  // disconnect and reconnect to whiteboard
  startSyncTest(16);
  // wait for connection
  while(!scribbleDoc->scribbleSync->isSyncActive())
    ScribbleApp::processEvents();
}

// Trace recording and replay for dzackgarza/math-notes-app tests/fixtures/write (see README.md there).
// The grammar is the set of actions the tests above perform: each helper writes one line, then acts, so that
//  recording runAll (WRITE_RECORD_DIR) converts each test to a trace, and replaying a trace from the same
//  setup reproduces the test's *_ref.html.  Input coordinates come from ScribbleInput::traceLog.

void ScribbleTest::startRecording(const std::string& path)
{
  ScribbleInput::traceLog = fopen(path.c_str(), "w");
  // the view at test start, as gotoPos arguments: the replayed test then sees the same screen
  Point pos = scribbleArea->dimToPageDim(scribbleArea->screenToDim(Point(0,0)));
  record("view %d %s %s", scribbleArea->currPageNum, ScribbleInput::traceReal(pos.x).c_str(),
      ScribbleInput::traceReal(pos.y).c_str());
}

void ScribbleTest::stopRecording()
{
  if(ScribbleInput::traceLog)
    fclose(ScribbleInput::traceLog);
  ScribbleInput::traceLog = NULL;
}

void ScribbleTest::record(const char* fmt, ...)
{
  if(!ScribbleInput::traceLog)
    return;
  va_list args;
  va_start(args, fmt);
  vfprintf(ScribbleInput::traceLog, fmt, args);
  va_end(args);
  fputc('\n', ScribbleInput::traceLog);
}

static std::string R(double x) { return ScribbleInput::traceReal(x); }

void ScribbleTest::setMode(int mode)
{
  record("mode %d", mode);
  scribbleMode->setMode(mode);
}

void ScribbleTest::setPen(const ScribblePen& p)
{
  record("pen %u %s %u %s %s %s %s %s %s", p.color.argb(), R(p.width).c_str(), p.flags, R(p.wRatio).c_str(),
      R(p.prParam).c_str(), R(p.spdMax).c_str(), R(p.dirAngle).c_str(), R(p.dash).c_str(), R(p.gap).c_str());
  scribbleDoc->app->setPen(p);
}

void ScribbleTest::setPageProperties(const PageProperties* p, bool applytoall, bool docdefault, bool global)
{
  record("props %s %s %s %s %s %u %u %d %d %d", R(p->width).c_str(), R(p->height).c_str(), R(p->xRuling).c_str(),
      R(p->yRuling).c_str(), R(p->marginLeft).c_str(), p->color.argb(), p->ruleColor.argb(),
      int(applytoall), int(docdefault), int(global));
  scribbleDoc->setPageProperties(p, applytoall, docdefault, global);
}

void ScribbleTest::gotoPos(int pagenum, Point pos)
{
  record("view %d %s %s", pagenum, R(pos.x).c_str(), R(pos.y).c_str());
  scribbleArea->gotoPos(pagenum, pos);
}

void ScribbleTest::gotoPage(int pagenum)
{
  record("page %d", pagenum);
  scribbleArea->gotoPage(pagenum);
}

void ScribbleTest::setScreenRect(const Rect& r)
{
  record("screen %s %s %s %s", R(r.left).c_str(), R(r.top).c_str(), R(r.width()).c_str(), R(r.height()).c_str());
  scribbleArea->screenRect = r;
}

void ScribbleTest::createHyperRef(const char* url)
{
  record("hyperref %s", url);
  scribbleArea->createHyperRef(url);
}

void ScribbleTest::createHyperRefToBookmark(Dim bookmarky)
{
  record("hyperref-bookmark %s", R(bookmarky).c_str());
  scribbleArea->createHyperRef(bookmarkArea->findBookmark(scribbleDoc->document, bookmarky));
}

void ScribbleTest::clearSelection()
{
  record("clearsel");
  scribbleArea->clearSelection();
}

void ScribbleTest::recentStrokeSelect()
{
  record("recentsel");
  scribbleArea->recentStrokeSelect();
}

// bookmarks must be painted before findBookmark can find them
void ScribbleTest::paintBookmarks()
{
  record("paintbookmarks");
  screenPaint->beginFrame();
  bookmarkArea->doPaintEvent(screenPaint);
  screenPaint->endFrame();
}

void ScribbleTest::setStrokeProperties(const StrokeProperties& props)
{
  record("strokeprops %u %s", props.color.argb(), R(props.width).c_str());
  scribbleArea->setStrokeProperties(props);
}

void ScribbleTest::setMoveSelMode(int mode)
{
  record("selmode %d", mode);
  scribbleMode->moveSelMode = mode;
}

void ScribbleTest::setPathDataRel(bool rel)
{
  record("pathrel %d", int(rel));
  SvgWriter::DEFAULT_PATH_DATA_REL = rel;
}

void ScribbleTest::setClipboardSvg(const char* svg)
{
  record("clipsvg %s", svg);
  SvgDocument* svgDoc = SvgParser().parseFragment(svg);
  Clipboard* clip = scribbleDoc->app->importExternalDoc(svgDoc);
  scribbleDoc->app->clipboard.reset(clip);
  scribbleDoc->app->clipboardPage = NULL;
}

void ScribbleTest::newDocument()
{
  record("newdoc");
  scribbleDoc->newDocument();
}

// save to path, then open it again; with loadall, load every page and delete the saved files
void ScribbleTest::reopen(const std::string& path, int saveflags, bool loadall)
{
  std::string ext = path.substr(path.find_last_of('.') + 1);
  record("reopen %s %d %d", ext.c_str(), saveflags, int(loadall));
  scribbleDoc->saveDocument(path.c_str(), Document::saveflags_t(saveflags));
  scribbleDoc->openDocument(path.c_str());
  if(loadall) {
    scribbleDoc->document->ensurePagesLoaded();
    scribbleDoc->document->deleteFiles();
  }
}

// replay

// Elements get ids in order of first appearance: input.html's elements in document order, then each element
//  as it appears after a trace line.  An element keeps its id while it is in the document, and on returning
//  (undo) if its bounds are unchanged; a new Element at the address of a freed one gets a new id.
struct ReplayIds
{
  struct Rec { int id; Rect bbox; };
  std::map<Element*, Rec> recs;
  std::set<int> live;
  int nextId = 0;

  void visit(Element* e, const std::set<int>& prevLive)
  {
    auto it = recs.find(e);
    if(it == recs.end() || (!prevLive.count(it->second.id) && it->second.bbox != e->bbox())) {
      recs[e] = Rec{nextId++, e->bbox()};
      it = recs.find(e);
    }
    it->second.bbox = e->bbox();
    live.insert(it->second.id);
    if(e->isMultiStroke()) {
      for(Element* c : e->children())
        visit(c, prevLive);
    }
  }

  void snapshot(Document* doc)
  {
    std::set<int> prevLive;
    prevLive.swap(live);
    for(Page* page : doc->pages) {
      if(page->loadStatus != Page::LOAD_OK)
        continue;
      for(Element* e : page->children())
        visit(e, prevLive);
    }
  }
};

static Timestamp replayTime = 0;
static Timestamp replayClock() { return replayTime; }

static std::vector<std::string> splitWords(const std::string& s, size_t n)
{
  // first n-1 whitespace separated words, then the rest of the line
  std::vector<std::string> w;
  size_t pos = 0;
  while(pos < s.size() && w.size() + 1 < n) {
    size_t start = s.find_first_not_of(" \t", pos);
    if(start == std::string::npos) break;
    size_t end = s.find_first_of(" \t", start);
    w.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
    pos = end == std::string::npos ? s.size() : end;
  }
  size_t rest = s.find_first_not_of(" \t", pos);
  if(rest != std::string::npos)
    w.push_back(s.substr(rest));
  return w;
}

bool ScribbleTest::replayLine(const std::string& line, const std::string& casedir)
{
  std::istringstream in(line);
  std::string op;
  in >> op;
  if(op.empty() || op[0] == '#')
    return true;
  if(op == "ie") {
    // scribbletest.cpp ie() at styluslabs/Write 401b65d, with (x, y) given relative to page 0 and t explicit
    Dim x, y, p;  int src, ev, mm;  long long t;
    if(!(in >> x >> y >> p >> src >> ev >> mm >> t)) return false;
    Point s = scribbleArea->dimToScreen(Point(x, y) + scribbleArea->getPageOrigin(0));
    replayTime = t;
    scribbleArea->scribbleInput->doInputEvent(s.x, s.y, p, (inputsource_t)src, (inputevent_t)ev, mm, t);
    return true;
  }
  if(op == "mt") {
    int ev1, ev2;  Dim x1, y1, x2, y2;
    if(!(in >> ev1 >> x1 >> y1 >> ev2 >> x2 >> y2)) return false;
    mtinput(inputevent_t(ev1), x1, y1, inputevent_t(ev2), x2, y2);
    return true;
  }
  if(op == "mode") { int m;  if(!(in >> m)) return false;  scribbleMode->setMode(m);  return true; }
  if(op == "cmd") { int c;  if(!(in >> c)) return false;  scribbleDoc->doCommand(c);  return true; }
  if(op == "pen") {
    unsigned int argb, flags;  Dim w, wr = 0, pr = 0, spd = 0, ang = 0, dash = 0, gap = 0;
    if(!(in >> argb >> w >> flags)) return false;
    in >> wr >> pr >> spd >> ang >> dash >> gap;
    scribbleDoc->app->setPen(ScribblePen(Color::fromArgb(argb), w, flags, wr, pr, spd, ang, dash, gap));
    return true;
  }
  if(op == "cfg") {
    std::string k, v;
    if(!(in >> k >> v)) return false;
    return scribbleDoc->cfg->setConfigValue(k.c_str(), v.c_str());
  }
  if(op == "props") {
    Dim w, h, xr, yr, ml;  unsigned int c, rc;  int all, docdef, global;
    if(!(in >> w >> h >> xr >> yr >> ml >> c >> rc >> all >> docdef >> global)) return false;
    PageProperties props(w, h, xr, yr, ml, Color::fromArgb(c), Color::fromArgb(rc));
    scribbleDoc->setPageProperties(&props, all, docdef, global);
    return true;
  }
  if(op == "view") {
    int pg;  Dim x, y;
    if(!(in >> pg >> x >> y)) return false;
    scribbleArea->gotoPos(pg, Point(x, y));
    return true;
  }
  if(op == "page") { int pg;  if(!(in >> pg)) return false;  scribbleArea->gotoPage(pg);  return true; }
  if(op == "screen") {
    Dim l, t, w, h;
    if(!(in >> l >> t >> w >> h)) return false;
    scribbleArea->screenRect = Rect::ltwh(l, t, w, h);
    return true;
  }
  if(op == "hyperref") {
    auto w = splitWords(line, 2);
    if(w.size() != 2) return false;
    scribbleArea->createHyperRef(w[1].c_str());
    return true;
  }
  if(op == "hyperref-bookmark") {
    Dim y;
    if(!(in >> y)) return false;
    scribbleArea->createHyperRef(bookmarkArea->findBookmark(scribbleDoc->document, y));
    return true;
  }
  if(op == "clearsel") { scribbleArea->clearSelection();  return true; }
  if(op == "recentsel") { scribbleArea->recentStrokeSelect();  return true; }
  if(op == "paintbookmarks") {
    screenPaint->beginFrame();
    bookmarkArea->doPaintEvent(screenPaint);
    screenPaint->endFrame();
    return true;
  }
  if(op == "strokeprops") {
    unsigned int argb;  Dim w;
    if(!(in >> argb >> w)) return false;
    scribbleArea->setStrokeProperties(StrokeProperties(Color::fromArgb(argb), w));
    return true;
  }
  if(op == "selmode") { int m;  if(!(in >> m)) return false;  scribbleMode->moveSelMode = m;  return true; }
  if(op == "pathrel") { int r;  if(!(in >> r)) return false;  SvgWriter::DEFAULT_PATH_DATA_REL = r;  return true; }
  if(op == "clipsvg") {
    auto w = splitWords(line, 2);
    if(w.size() != 2) return false;
    Clipboard* clip = scribbleDoc->app->importExternalDoc(SvgParser().parseFragment(w[1].c_str()));
    scribbleDoc->app->clipboard.reset(clip);
    scribbleDoc->app->clipboardPage = NULL;
    return true;
  }
  if(op == "newdoc") { scribbleDoc->newDocument();  return true; }
  if(op == "reopen") {
    std::string ext;  int flags, loadall;
    if(!(in >> ext >> flags >> loadall)) return false;
    std::string path = replayTmpDir + "/replay." + ext;
    scribbleDoc->saveDocument(path.c_str(), Document::saveflags_t(flags));
    scribbleDoc->openDocument(path.c_str());
    if(loadall) {
      scribbleDoc->document->ensurePagesLoaded();
      scribbleDoc->document->deleteFiles();
    }
    return true;
  }
  return false;
}

static void writeTransform(std::ostream& out, const Transform2D& tf)
{
  out << "\"dx\": " << R(tf.xoffset()) << ", \"dy\": " << R(tf.yoffset()) << ", \"transform\": [";
  for(int ii = 0; ii < 6; ++ii)
    out << (ii ? ", " : "") << R(tf.m[ii]);
  out << "]";
}

static void writeElement(std::ostream& out, Element* e, ReplayIds& ids, const Transform2D& parenttf,
    bool penpoints, const std::string& indent)
{
  const Transform2D& tf = e->node->getTransform();
  out << indent << "{\"id\": " << ids.recs[e].id << ", ";
  writeTransform(out, tf);
  if(penpoints && e->isPathElement()) {
    // Element::toPenPoints gives the centerline in the element's own coordinates; write it in page units
    Transform2D total = parenttf * tf;
    out << ",\n" << indent << " \"penPoints\": [";
    bool first = true;
    for(const PenPoint& pp : ScribbleTest::penPoints(e)) {
      if(pp.moveTo())
        out << (first ? "[" : "], [");
      else
        out << ", ";
      Point p = total.map(pp.p);
      out << "[" << R(p.x) << ", " << R(p.y) << "]";
      first = false;
    }
    out << (first ? "]" : "]]");
  }
  if(e->isMultiStroke()) {
    out << ",\n" << indent << " \"children\": [\n";
    bool first = true;
    for(Element* c : e->children()) {
      out << (first ? "" : ",\n");
      writeElement(out, c, ids, parenttf * tf, penpoints, indent + "  ");
      first = false;
    }
    out << "]";
  }
  out << "}";
}

static void writeIdList(std::ostream& out, const std::set<int>& ids)
{
  out << "[";
  bool first = true;
  for(int id : ids) { out << (first ? "" : ", ") << id;  first = false; }
  out << "]";
}

// Replay casedir/trace.txt on casedir/input.html (if present) and write, next to them, each page as
//  result-p<N>.svg (Page::saveSVGFile, native transforms kept) and expected.json.  A case directory named
//  upstream-test<N> is also saved as runAll saves test<N> and compared with SCRIBBLE_TEST_PATH/test<N>_ref.html.
void ScribbleTest::replay(const std::string& casedir)
{
  std::vector<char> tracebuf;
  if(!readFile(&tracebuf, (casedir + "/trace.txt").c_str()) || tracebuf.empty()) {
    ++nFailed;
    resultStr += "\n" + casedir + ": cannot read trace.txt";
    return;
  }
  srandpp(1);
  SvgWriter::DEFAULT_PATH_DATA_REL = false;
  doCommand(ID_RESETZOOM);
  setupTest(casedir + "/input.html");
  scribbleArea->gotoPos(0, Point(0,0));

  ReplayIds ids;
  ids.snapshot(scribbleDoc->document);
  bool penpoints = false;
  std::istringstream lines(std::string(tracebuf.begin(), tracebuf.end()));
  std::string line;
  for(int lineno = 1; std::getline(lines, line); ++lineno) {
    if(!replayLine(line, casedir)) {
      ++nFailed;
      resultStr += fstring("\n%s/trace.txt:%d: cannot replay: %s", casedir.c_str(), lineno, line.c_str());
      return;
    }
    penpoints = penpoints || StringRef(line).startsWith(fstring("mode %d", MODE_ERASEFREE).c_str());
    ids.snapshot(scribbleDoc->document);
  }

  Document* doc = scribbleDoc->document;
  doc->ensurePagesLoaded();
  std::set<int> selected;
  std::ostringstream json;
  json << "{\n\"pages\": [";
  for(size_t pp = 0; pp < doc->pages.size(); ++pp) {
    Page* page = doc->pages[pp];
    page->saveSVGFile(fstring("%s/result-p%d.svg", casedir.c_str(), int(pp)).c_str());
    json << (pp ? ",\n" : "\n") << " {\"elements\": [\n";
    bool first = true;
    for(Element* e : page->children()) {
      json << (first ? "" : ",\n");
      writeElement(json, e, ids, Transform2D(), penpoints, "  ");
      first = false;
      if(scribbleArea->currSelection && e->isSelected(scribbleArea->currSelection))
        selected.insert(ids.recs[e].id);
    }
    json << "]}";
  }
  std::set<int> deleted;
  for(int id = 0; id < ids.nextId; ++id)
    if(!ids.live.count(id)) deleted.insert(id);
  json << "],\n\"selected\": ";
  writeIdList(json, selected);
  json << ",\n\"deleted\": ";
  writeIdList(json, deleted);
  json << "\n}\n";
  std::string jsonstr = json.str();
  std::ofstream jsonfile((casedir + "/expected.json").c_str(), std::ios::binary);
  jsonfile.write(jsonstr.data(), jsonstr.size());
  jsonfile.close();

  int testnum = -1;
  std::string name = FSPath(casedir).fileName();
  if(sscanf(name.c_str(), "upstream-test%d", &testnum) == 1) {
    std::string outfile = replayTmpDir + fstring("/test%d_out.html", testnum);
    saveTestOutput(outfile);
    std::string reffile = fstring("%s/test%d_ref.html", SCRIBBLE_TEST_PATH, testnum);
    if(testCompareFiles(outfile.c_str(), reffile.c_str(), true))
      scribbleDoc->document->deleteFiles();
    else {
      ++nFailed;
      resultStr += "\n" + name + ": replay does not reproduce " + reffile + " (output kept at " + outfile + ")";
    }
  }
}

void ScribbleTest::replayAll(const std::string& casesdir)
{
  nFailed = 0;
  resultStr.clear();
  // global setup as in runAll
  Dim unitsPerPx = ScribbleView::unitsPerPx;
  ScribbleView::unitsPerPx = 1;
  SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = 1;
  Page::BLANK_Y_RULING = scribbleConfig->Float("blankYRuling");
  Element::SVG_NO_TIMESTAMP = true;
  Page::clock = replayClock;
  const char* tmp = getenv("WRITE_REPLAY_TMP");
  replayTmpDir = tmp ? tmp : outPath;

  std::vector<std::string> cases = lsDirectory(FSPath(casesdir));
  std::sort(cases.begin(), cases.end());
  int ncases = 0;
  for(const std::string& c : cases) {
    if(c.empty() || c.back() != '/') continue;
    replay(FSPath(casesdir, c).filePath());
    ++ncases;
  }

  Page::clock = mSecSinceEpoch;
  srandpp(mSecSinceEpoch());
  SvgWriter::DEFAULT_PATH_DATA_REL = true;
  Element::SVG_NO_TIMESTAMP = false;
  ScribbleView::unitsPerPx = unitsPerPx;
  ScribbleApp::app->loadConfig();
  resultStr = fstring("Replayed %d cases from %s with %d failures.", ncases, casesdir.c_str(), nFailed) + resultStr;
  if(exitAfterTest) {
    SCRIBBLE_LOG(resultStr.c_str());
    exit(nFailed ? 1 : 0);
  }
}
