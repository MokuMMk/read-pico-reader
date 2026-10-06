// SPDX-License-Identifier: Apache-2.0
// 串行服务测试协议替身，不联网。/ Service protocol fake; no network.
#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
struct HalFile {};
inline std::vector<std::string> fake_ids={"a","b","c"}, fake_started;
inline bool fake_cancel, fake_reset_cancel;
inline void (*fake_after_reset)();
inline unsigned fake_active_operations, fake_peak_operations;
namespace WeReadStore {
struct ShelfRecord {char bookId[64]={},title[192]={},author[96]={},coverUrl[512]={};};
struct Session {bool valid()const{return true;}void clear(){}};
inline bool loadSession(Session&){return true;}
inline bool clearSession(){return true;}inline bool clearShelf(){return true;}
inline bool openShelf(HalFile&,uint32_t& n){n=fake_ids.size();return true;}
inline bool readShelfRecord(HalFile&,unsigned i,ShelfRecord& r){if(i>=fake_ids.size())return false;r={};snprintf(r.bookId,64,"%s",fake_ids[i].c_str());snprintf(r.title,192,"title-%s",r.bookId);return true;}
inline std::string finalBookPath(const ShelfRecord& r){return std::string("/WeRead/")+r.title+".epub";}
enum class ImagePolicy{Embed,Exclude};
}
namespace WeReadBrowse {inline bool clearAllCaches(){return true;}}
namespace WeReadClient {
enum class Error{Ok,Cancelled,Network,SessionExpired,LoginFailed,Protocol,SdCard,Integrity,Unavailable,Clock,OutOfMemory,WholeBookOnly,CoverUnavailable};
struct DownloadOptions{WeReadStore::ImagePolicy imagePolicy;};
class Operation {
 std::string id;bool running=false;unsigned steps=0;
public:
 enum class Kind{Sync,Download};enum class ProgressStage{Chapters,Preparing,Images,Packaging};
 enum class Event{None,QrReady,Authenticated,Complete,Failed,Cancelled};
 ~Operation(){reset();}
 bool begin(Kind,const WeReadStore::ShelfRecord* book,DownloadOptions){assert(!running&&!fake_active_operations);id=book?book->bookId:"sync";fake_started.push_back(id);running=true;steps=0;++fake_active_operations;if(fake_active_operations>fake_peak_operations)fake_peak_operations=fake_active_operations;return true;}
 bool active()const{return running;}
 void cancel(){fake_cancel=true;}
 void reset(){if(running)--fake_active_operations;running=false;if(fake_reset_cancel){fake_cancel=true;fake_reset_cancel=false;if(fake_after_reset)fake_after_reset();}}
 Event step(void(*callback)(void*),void* ctx){callback(ctx);if(fake_cancel)return Event::Cancelled;if(++steps<2)return Event::None;return id=="b"?Event::Failed:Event::Complete;}
 Error error()const{return Error::CoverUnavailable;}
 const char* qrUrl()const{return "";}const char* finalPath()const{return "/WeRead/final.epub";}
 unsigned progressCompleted()const{return steps;}unsigned progressTotal()const{return 2;}unsigned skippedImageCount()const{return 0;}
 ProgressStage progressStage()const{return ProgressStage::Chapters;}
};
}
