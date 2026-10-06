// SPDX-License-Identifier: Apache-2.0
// 真实下载状态机：关闭插图也下载封面，并验证封面进入成品 EPUB。
// Real download state machine: retain covers with illustrations disabled and package them into EPUB.
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include "nvs.h"
#include "../components/pico_weread/vendor/WeReadClient.cpp"
static std::vector<unsigned char> session_blob;
bool pico_weread_cancelled(){return false;}
esp_err_t nvs_open(const char*,nvs_open_mode_t,nvs_handle_t* h){*h=1;return 0;}
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void* out,size_t* n){if(session_blob.empty()||*n<session_blob.size())return 1;memcpy(out,session_blob.data(),session_blob.size());*n=session_blob.size();return 0;}
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void* p,size_t n){auto c=(const unsigned char*)p;session_blob.assign(c,c+n);return 0;}
esp_err_t nvs_commit(nvs_handle_t){return 0;}void nvs_close(nvs_handle_t){}esp_err_t nvs_erase_key(nvs_handle_t,const char*){session_blob.clear();return 0;}
static const uint8_t png[]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,4,0,0,0,181,28,12,2,0,0,0,11,73,68,65,84,120,218,99,252,255,31,0,3,3,2,0,239,191,173,163,0,0,0,0,73,69,78,68,174,66,96,130};
static unsigned cover_requests;static bool bad_cover;
namespace WeReadHttpClient {
Session::~Session(){}bool Session::reusable(){return false;}void Session::reset(){}void Session::clearStats(){}
bool networkReady(){return true;}
bool parseHttpsUrl(const char* url,HttpsUrlView& v){if(strncmp(url,"https://",8))return false;v.host=url+8;const char* p=strchr(v.host,'/');v.hostLength=p?(size_t)(p-v.host):strlen(v.host);v.path=p?p:"/";return v.hostLength>0;}
bool extractHttpsHost(const char* u,char* h,size_t n){HttpsUrlView v;if(!parseHttpsUrl(u,v)||v.hostLength>=n)return false;memcpy(h,v.host,v.hostLength);h[v.hostLength]=0;return true;}
Result request(const char* url,const RequestOptions&,const DataCallback& data,const HeaderCallback& headers,int& status){
 assert(strstr(url,"cover.png"));++cover_requests;status=bad_cover?404:200;headers("content-type","image/png");
 return bad_cover||data(png,sizeof(png))?Result::Ok:Result::Aborted;
}
Result request(Session&,const char* u,const RequestOptions& o,const DataCallback& d,const HeaderCallback& h,int& s){return request(u,o,d,h,s);}
}
namespace WeReadClient {
struct OperationTestPeer {
 static bool toc(const Operation& op){return op.phase_==Operation::Phase::FetchToc;}
 static void package(Operation& op){op.phase_=Operation::Phase::PackageBook;op.chapterCount_=1;op.firstChapterIndex_=op.lastChapterIndex_=0;}
 static void skipCover(Operation& op){op.phase_=Operation::Phase::PrepareImages;}
 static void assertPolicy(const Operation& op){assert(op.options_.imagePolicy==WeReadStore::ImagePolicy::Exclude);}
};
}
int main(int argc,char** argv){
 assert(argc==2);Storage.configure((std::string(argv[1])+"/cache").c_str(),(std::string(argv[1])+"/books").c_str());
 WeReadStore::Session session;strcpy(session.vid,"fake");strcpy(session.skey,"fake");assert(WeReadStore::saveSession(session));
 WeReadStore::ShelfRecord book;strcpy(book.bookId,"123");strcpy(book.title,"测试原名书籍");strcpy(book.author,"作者");strcpy(book.coverUrl,"https://res.weread.qq.com/cover.png");
 WeReadClient::Operation op;WeReadClient::DownloadOptions options;options.imagePolicy=WeReadStore::ImagePolicy::Exclude;
 assert(op.begin(WeReadClient::Operation::Kind::Download,&book,options));
 for(unsigned i=0;i<8&&!WeReadClient::OperationTestPeer::toc(op);++i)assert(op.step()!=WeReadClient::Operation::Event::Failed);
 assert(WeReadClient::OperationTestPeer::toc(op)&&cover_requests==1);WeReadClient::OperationTestPeer::assertPolicy(op);
 auto dir=WeReadStore::bookDirectory(book.bookId);std::string cover;assert(WeReadClient::findCoverSource(dir,cover)==WeReadProtocol::ImageType::Png);
 WeReadStore::IndexWriter toc;assert(toc.begin(WeReadStore::tocPath(book.bookId),WeReadStore::kTocMagic,sizeof(WeReadStore::TocRecord)));
 WeReadStore::TocRecord chapter;strcpy(chapter.chapterUid,"1");strcpy(chapter.title,"第一章");assert(toc.append(&chapter)&&toc.finish());
 HalFile body;assert(Storage.openFileForWrite("test",WeReadStore::chapterPath(dir,0),body));const char* text="<html><body><p>正文</p></body></html>";assert(body.write(text,strlen(text))==strlen(text));body.close();
 WeReadClient::OperationTestPeer::package(op);assert(op.step()==WeReadClient::Operation::Event::Complete);
 assert(std::string(op.finalPath())=="/WeRead/测试原名书籍.epub");assert(WeReadStore::looksLikeZip(op.finalPath()));
 // 清空源图复现下载失败，不能静默发布无封面文件。/ A failed cover must not silently publish a coverless book.
 assert(Storage.remove(cover.c_str()));bad_cover=true;assert(op.begin(WeReadClient::Operation::Kind::Download,&book,options));
 for(unsigned i=0;i<8&&op.active();++i)op.step();assert(op.error()==WeReadClient::Error::CoverUnavailable);
 puts("PASS: real cover state machine, mandatory cover with excluded inline images, EPUB package and cover failure");
}
