// Fixed helper protocol. Filesystem work never blocks the SDL/CEF UI thread.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>
extern char** environ;
struct DesktopJob { int id; unsigned epoch; std::string request, response; };
class DesktopWorker {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<DesktopJob> queue, replies;
  std::thread thread;
  int input=-1, output=-1;
  pid_t pid=-1;
  std::atomic<bool> stopping{false};
  bool failed=false;
  size_t pending=0;
  bool Transfer(const std::string& request,std::string& response) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(900);
    std::string wire=request+'\n';size_t offset=0;response.clear();
    while(!stopping&&std::chrono::steady_clock::now()<deadline) {
      bool writing=offset<wire.size();pollfd descriptor{writing?input:output,static_cast<short>(writing?POLLOUT:POLLIN),0};
      int ready=poll(&descriptor,1,100);
      if(ready<0){if(errno==EINTR)continue;return false;}
      if(!ready)continue;
      if(descriptor.revents&(POLLERR|POLLNVAL))return false;
      if(writing) {
        ssize_t count=write(input,wire.data()+offset,wire.size()-offset);
        if(count>0)offset+=static_cast<size_t>(count);
        else if(count<0&&errno!=EAGAIN&&errno!=EINTR)return false;
      } else {
        char bytes[4096];ssize_t count=read(output,bytes,sizeof(bytes));
        if(count<=0){if(count<0&&(errno==EAGAIN||errno==EINTR))continue;return false;}
        for(ssize_t i=0;i<count;++i){if(bytes[i]=='\n')return i==count-1;if(response.size()>=1500000)return false;response.push_back(bytes[i]);}
      }
    }
    return false;
  }
 public:
  std::atomic<unsigned> epoch{0};
  bool Start(const std::filesystem::path& executable, const std::filesystem::path& profile) {
    int to_child[2], from_child[2];
    if (pipe2(to_child,O_CLOEXEC)) return false;
    if (pipe2(from_child,O_CLOEXEC)) { close(to_child[0]); close(to_child[1]); return false; }
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,to_child[0],STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions,from_child[1],STDOUT_FILENO);
    std::string script=(executable/"desktop_backend.py").string(), path=profile.string();
    char* argv[]={const_cast<char*>("/usr/bin/python3"),const_cast<char*>("-I"),const_cast<char*>("-B"),script.data(),path.data(),nullptr};
    int rc=posix_spawn(&pid,"/usr/bin/python3",&actions,nullptr,argv,environ);
    posix_spawn_file_actions_destroy(&actions); close(to_child[0]);close(from_child[1]);
    if (rc) { close(to_child[1]);close(from_child[0]);pid=-1;return false; }
    input=to_child[1];output=from_child[0];
    fcntl(input,F_SETFL,fcntl(input,F_GETFL)|O_NONBLOCK);fcntl(output,F_SETFL,fcntl(output,F_GETFL)|O_NONBLOCK);
    thread=std::thread([this] {
      for (;;) {
        DesktopJob job;
        { std::unique_lock<std::mutex> guard(mutex); wake.wait(guard,[this]{return stopping||!queue.empty();});
          if(queue.empty()&&stopping) break;
          job=std::move(queue.front());queue.pop_front(); }
        if (job.id && job.epoch!=epoch.load()) { std::lock_guard<std::mutex> guard(mutex);--pending;continue; }
        bool ok=!failed&&Transfer(job.request,job.response);
        if(!ok)failed=true; // A timed-out stream cannot safely be reused.
        if(!ok)job.response="{\"ok\":false,\"error\":\"Desktop storage worker unavailable; normal loading can continue\"}";
        { std::lock_guard<std::mutex> guard(mutex);--pending;if(job.id)replies.push_back(std::move(job)); }
      }
    });return true;
  }
  bool Submit(int id,unsigned context,const std::string& request) {
    std::lock_guard<std::mutex> guard(mutex);
    if(stopping||!thread.joinable()||pending>=16||request.size()>1500000)return false;
    ++pending;queue.push_back({id,context,request,{}});wake.notify_one();return true;
  }
  void Reset() {
    ++epoch;
    std::lock_guard<std::mutex> guard(mutex);
    pending-=queue.size();queue.clear();replies.clear();
    if(thread.joinable()&&!stopping){++pending;queue.push_back({0,epoch.load(),"{\"op\":\"reset\",\"args\":[]}",{}});wake.notify_one();}
  }
  std::deque<DesktopJob> Take() { std::lock_guard<std::mutex> guard(mutex);std::deque<DesktopJob> result;result.swap(replies);return result; }
  void Stop() {
    { std::lock_guard<std::mutex> guard(mutex);stopping=true;wake.notify_one(); }
    if(thread.joinable())thread.join();
    if(input>=0){close(input);input=-1;}
    if(output>=0){close(output);output=-1;}
    if(pid>0){
      int status;bool reaped=false;
      for(int i=0;i<20;++i){pid_t result=waitpid(pid,&status,WNOHANG);if(result==pid||(result<0&&errno==ECHILD)){reaped=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(50));}
      // Only this instance's helper; never an existing client or unrelated process.
      if(!reaped){kill(pid,SIGTERM);for(int i=0;i<20;++i){if(waitpid(pid,&status,WNOHANG)==pid){reaped=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(50));}}
      if(!reaped){kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}}
      pid=-1;
    }
  }
  ~DesktopWorker(){Stop();}
};
