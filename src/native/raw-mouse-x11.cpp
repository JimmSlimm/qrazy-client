// XInput2 driver-untransformed relative valuators; never browser movement deltas.
// Raw events are root-only in XI2. Selection exists only during focused capture.
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <utility>
static Display* display;
static Window target, root;
static int opcode, master;
static bool active = false;
static unsigned long generation;
static std::map<int, std::pair<int,int>> axes;
static double clockMs() { timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec*1000.0+t.tv_nsec/1000000.0; }
// Protocol errors fail closed instead of leaving a partially initialized selection.
static int xerror(Display*, XErrorEvent*) { std::_Exit(2); }
static bool focused() {
  XWindowAttributes a{};
  if (!XGetWindowAttributes(display,target,&a) || a.map_state != IsViewable) return false;
  Window focus; int revert; XGetInputFocus(display,&focus,&revert);
  for (int i=0; i<64 && focus!=None && focus!=PointerRoot; ++i) {
    if (focus==target) return true;
    Window r,p,*children=nullptr; unsigned int n;
    if (!XQueryTree(display,focus,&r,&p,&children,&n)) return false;
    if (children) XFree(children);
    if (focus==p) break;
    focus=p;
  }
  return false;
}
static void selectRaw(bool enabled) {
  unsigned char bits[XIMaskLen(XI_LASTEVENT)]{};
  if (enabled) { XISetMask(bits,XI_RawMotion); XISetMask(bits,XI_DeviceChanged); }
  XIEventMask mask{master, int(sizeof(bits)), bits};
  unsigned char topology[XIMaskLen(XI_LASTEVENT)]{};
  if (enabled) XISetMask(topology,XI_HierarchyChanged);
  XIEventMask topologyMask{XIAllDevices,int(sizeof(topology)),topology};
  XISelectEvents(display,root,&mask,1); XISelectEvents(display,root,&topologyMask,1); XSync(display,False);
  active=enabled;
}
static bool queryAxes() {
  axes.clear(); int count=0;
  XIDeviceInfo* devices=XIQueryDevice(display,XIAllDevices,&count);
  if (!devices) return false;
  const Atom x=XInternAtom(display,"Rel X",False), y=XInternAtom(display,"Rel Y",False);
  for (int i=0;i<count;++i) {
    const auto& d=devices[i];
    if (d.use!=XISlavePointer || d.attachment!=master || !d.enabled) continue;
    int ax=-1,ay=-1;
    for (int c=0;c<d.num_classes;++c) if (d.classes[c]->type==XIValuatorClass) {
      const auto* v=reinterpret_cast<XIValuatorClassInfo*>(d.classes[c]);
      if (v->mode!=XIModeRelative) continue;
      if (v->label==x) ax=v->number;
      if (v->label==y) ay=v->number;
    }
    if (ax>=0 && ay>=0) axes[d.deviceid]={ax,ay};
  }
  XIFreeDeviceInfo(devices); return !axes.empty();
}
int main(int argc,char** argv) {
  if (argc!=3 || getenv("WAYLAND_DISPLAY") || getenv("WAYLAND_SOCKET")) return 2;
  target=std::strtoul(argv[1],nullptr,10);
  display=XOpenDisplay(nullptr); if (!display) return 2;
  XSetErrorHandler(xerror);
  int event,error;
  // Do not infer XWayland equivalence from XI2 availability.
  if (XQueryExtension(display,"XWAYLAND",&opcode,&event,&error)) return 2;
  if (!XQueryExtension(display,"XInputExtension",&opcode,&event,&error)) return 2;
  int major=2,minor=1;
  if (XIQueryVersion(display,&major,&minor)!=Success || major<2 || (major==2 && minor<1)) return 2;
  XWindowAttributes attr{}; if (!XGetWindowAttributes(display,target,&attr)) return 2;
  root=attr.root;
  Atom actual; int format; unsigned long count,remaining; unsigned char* bytes=nullptr;
  const Atom pidAtom=XInternAtom(display,"_NET_WM_PID",True);
  if (!pidAtom || XGetWindowProperty(display,target,pidAtom,0,1,False,XA_CARDINAL,&actual,&format,&count,&remaining,&bytes)!=Success) return 2;
  const bool owner=bytes && actual==XA_CARDINAL && format==32 && count==1 && *reinterpret_cast<unsigned long*>(bytes)==std::strtoul(argv[2],nullptr,10);
  if (bytes) XFree(bytes);
  if (!owner || !XIGetClientPointer(display,target,&master)) return 2;
  XSelectInput(display,target,StructureNotifyMask|FocusChangeMask);
  // No root input or hierarchy subscription until capture. Topology/class
  // changes during capture fail closed; retry rebuilds the relative-axis map.
  XFlush(display);
  std::string input;
  std::cout.precision(17);
  for (;;) {
    pollfd fds[2]{{STDIN_FILENO,POLLIN,0},{ConnectionNumber(display),POLLIN,0}};
    const int result=poll(fds,2,15); if (result<0) break;
    if (fds[0].revents&(POLLHUP|POLLERR)) break;
    if (fds[0].revents&POLLIN) {
      char data[256]; const auto n=read(STDIN_FILENO,data,sizeof(data)); if (n<=0) break;
      input.append(data,size_t(n)); if (input.size()>4096) break;
      size_t end;
      while ((end=input.find('\n'))!=std::string::npos) {
        const std::string line=input.substr(0,end); input.erase(0,end+1);
        std::istringstream command(line); char kind=0; unsigned long id=0;
        command>>kind;
        if (kind=='T' && command>>id) std::cout<<"{\"id\":"<<id<<",\"ok\":true,\"time\":"<<clockMs()<<"}\n"<<std::flush;
        else if (kind=='C' && command>>generation>>id) {
          if (active) selectRaw(false);
          // Discard stale events before the next capture generation.
          XSync(display,True);
          const bool ok=focused() && queryAxes();
          if (ok) selectRaw(true);
          std::cout<<"{\"id\":"<<id<<",\"ok\":"<<(ok?"true":"false")<<"}\n"<<std::flush;
        } else return 2;
      }
    }
    if (active && !focused()) { selectRaw(false); std::cout<<"{\"release\":true}\n"<<std::flush; break; }
    // Bound each drain so input/release cannot starve behind a high polling-rate mouse.
    for (int i=0;i<512 && XPending(display);++i) {
      XEvent e; XNextEvent(display,&e);
      if (e.type==DestroyNotify || e.type==UnmapNotify || (e.type==FocusOut && e.xfocus.detail!=NotifyInferior)) return 2;
      if (e.type!=GenericEvent || e.xcookie.extension!=opcode || !XGetEventData(display,&e.xcookie)) continue;
      const bool classesChanged=e.xcookie.evtype==XI_DeviceChanged &&
        static_cast<XIDeviceChangedEvent*>(e.xcookie.data)->reason==XIDeviceChange;
      // A normal master/slave source switch does not invalidate the source-ID
      // axis map. Genuine device class changes or topology changes do.
      if ((e.xcookie.evtype==XI_HierarchyChanged || classesChanged) && active) { XFreeEventData(display,&e.xcookie); return 2; }
      if (active && e.xcookie.evtype==XI_RawMotion && focused()) {
        const auto* raw=static_cast<XIRawEvent*>(e.xcookie.data);
        const auto found=axes.find(raw->sourceid);
        if (raw->deviceid==master && !(raw->flags&XIPointerEmulated) && found!=axes.end()) {
          double dx=0,dy=0; int index=0;
          for (int axis=0;axis<raw->valuators.mask_len*8;++axis) if (XIMaskIsSet(raw->valuators.mask,axis)) {
            const double value=raw->raw_values[index++];
            if (axis==found->second.first) dx=value;
            if (axis==found->second.second) dy=value;
          }
          if (std::isfinite(dx) && std::isfinite(dy) && (dx || dy))
            std::cout<<"{\"dx\":"<<dx<<",\"dy\":"<<dy<<",\"time\":"<<clockMs()<<",\"generation\":"<<generation<<"}\n"<<std::flush;
        }
      }
      XFreeEventData(display,&e.xcookie);
    }
    if (fds[1].revents&(POLLHUP|POLLERR)) break;
  }
  XCloseDisplay(display); return 0;
}
