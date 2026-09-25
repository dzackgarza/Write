#ifndef SCRIBBLETEST_H
#define SCRIBBLETEST_H

#include "scribbledoc.h"
#include "bookmarkview.h"

class ScribbleTest
{
friend class ScribbleApp;
public:
  ScribbleTest(const std::string &path);
  ScribbleTest(ScribbleDoc* sd, BookmarkView* bv, ScribbleMode* sm);
  ~ScribbleTest();

  void runAll(bool runsynctest = false);
  void performanceTest();
  void inputTest();
  void syncSlaveMsg(std::string msg, int level);
  // replay every case directory under casesdir (see replay() in scribbletest.cpp)
  void replayAll(const std::string& casesdir);

  // result string to be read by caller
  std::string resultStr;

  static bool exitAfterTest;

private:
  ScribbleConfig* scribbleConfig;
  ScribbleDoc* scribbleDoc;
  ScribbleArea* scribbleArea;
  ScribbleMode* scribbleMode;
  BookmarkView* bookmarkArea;
  Image* screenImg;
  Painter* screenPaint;
  Rect screenRect;
  std::string outPath;
  int nFailed;

  // shared whiteboard testing
  pugi::xml_document swbXML;
  ScribbleTest* syncSlave;
  int syncTestNum;
  bool waitForSyncDone;
  void startSyncTest(int testnum);
  void waitForSync();
  void checkStrokeMap(ScribbleDoc* doc, const char* msg);

  static const int pen = INPUTSOURCE_PEN;
  static const int press = INPUTEVENT_PRESS;
  static const int release = INPUTEVENT_RELEASE;
  void undo() { doCommand(ID_UNDO); }
  void redo() { doCommand(ID_REDO); }
  void doCommand(int cmd) { record("cmd %d", cmd);  scribbleDoc->doCommand(cmd); }
  bool testCompareFiles(const char* f1, const char* f2, bool svgonly = false);
  void ie(Dim x, Dim y, Dim p, int src, int ev = 0, int mm = 0);
  void mtinput(inputevent_t ev1, Dim x1, Dim y1, inputevent_t ev2, Dim x2, Dim y2);
  void ss(Dim offset);
  void s1(Dim xoffset, Dim yoffset);
  void s2(Dim xoffset, Dim yoffset);
  void s3(Dim xoffset, Dim yoffset);
  void f2(Dim xoffset, Dim yoffset);
  void hr(Dim xoffset, Dim yoffset);
  void s3();
  void s4();

  void setupTest(const std::string& inpath);
  void saveTestOutput(const std::string& outfile);

  // trace recording: each helper below writes its replay-grammar line (when recording), then acts
  void startRecording(const std::string& path);
  void stopRecording();
  void record(const char* fmt, ...);
  void setMode(int mode);
  void setPen(const ScribblePen& pen);
  template<typename T> void cfgSet(const char* key, T value)
      { record("cfg %s %s", key, std::to_string(value).c_str());  scribbleDoc->cfg->set(key, value); }
  void setPageProperties(const PageProperties* props, bool applytoall, bool docdefault, bool global);
  void gotoPos(int pagenum, Point pos);
  void gotoPage(int pagenum);
  void setScreenRect(const Rect& r);
  void createHyperRef(const char* url);
  void createHyperRefToBookmark(Dim bookmarky);
  void clearSelection();
  void recentStrokeSelect();
  void paintBookmarks();
  void setStrokeProperties(const StrokeProperties& props);
  void setMoveSelMode(int mode);
  void setPathDataRel(bool rel);
  void setClipboardSvg(const char* svg);
  void newDocument();
  void reopen(const std::string& path, int saveflags, bool loadall);

  // replay
  void replay(const std::string& casedir);
  bool replayLine(const std::string& line, const std::string& casedir);
  std::string replayTmpDir;
public:
  static std::vector<PenPoint> penPoints(Element* e) { return e->toPenPoints(); }

  void test0();
  void test1();
  void test2();
  void test3();
  void test4();
  void test5();
  void test6();
  void test7();
  void test8();
  void test9();
  void test10();
  void test11();
  void test12();
  void test13();
  void test14();
  void test15();
  void synctest01();
  void synctest01slave1();
  void synctest01slave2();
};

#endif
