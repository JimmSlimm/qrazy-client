#pragma once
struct ProfileLock {
  int fd=-1;
  ~ProfileLock(){if(fd>=0)close(fd);}
  bool Acquire(const std::filesystem::path& profile) {
    const auto path=profile/"host.lock";
    const char* inherited=std::getenv("QRAZY_LOCK_FD");
    if(inherited) {
      std::string descriptor=inherited;
      char* end=nullptr;long number=std::strtol(descriptor.c_str(),&end,10);
      unsetenv("QRAZY_LOCK_FD");
      if(descriptor.empty()||*end||number<3||number>std::numeric_limits<int>::max())return false;
      int candidate=static_cast<int>(number);struct stat held{},named{};
      if(fstat(candidate,&held)||lstat(path.c_str(),&named)||!S_ISREG(held.st_mode)||
         !S_ISREG(named.st_mode)||held.st_dev!=named.st_dev||held.st_ino!=named.st_ino||
         held.st_uid!=getuid()||flock(candidate,LOCK_EX|LOCK_NB)||fcntl(candidate,F_SETFD,FD_CLOEXEC))return false;
      fd=candidate;return true;
    }
    fd=open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    return fd>=0&&flock(fd,LOCK_EX|LOCK_NB)==0;
  }
};
