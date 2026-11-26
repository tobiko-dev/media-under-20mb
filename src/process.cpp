#include "media.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace drop {
void Hooks::check() const { if (cancel.load()) throw Cancelled(); }
class Lines {
    const Line& receive;
    std::string current, tail;
public:
    explicit Lines(const Line& f) : receive(f) {}
    void flush() {
        if (current.empty()) return;
        receive(current);
        // Retain actual errors, excluding probe output and FFmpeg's progress records.
        if (current.rfind("streams.",0) != 0 && current.rfind("format.",0) != 0 &&
            current.rfind("stream_index=",0) != 0 &&
            (current.find('=') == std::string::npos || current.front() == '[')) {
            if (!tail.empty()) tail += "\n";
            tail += current;
            if (tail.size() > 2400) tail.erase(0,tail.size()-2400);
        }
        current.clear();
    }
    void add(const char* data, size_t n) {
        for (size_t i=0; i<n; ++i) {
            if (data[i]=='\n' || data[i]=='\r') flush();
            else if (current.size()<65536) current += data[i];
        }
    }
    std::string finish() { flush(); return tail; }
};
#ifdef _WIN32
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);
    std::wstring out(n,0); MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),out.data(),n); return out;
}
std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    std::string out(n,0); WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),out.data(),n,nullptr,nullptr); return out;
}
struct Handle {
    HANDLE h=nullptr;
    ~Handle() { close(); }
    void close() { if (h && h!=INVALID_HANDLE_VALUE) CloseHandle(h); h=nullptr; }
};
static std::wstring quote(const std::wstring& arg) {
    std::wstring out=L"\""; size_t slashes=0;
    for (wchar_t c : arg) {
        if (c==L'\\') { ++slashes; continue; }
        if (c==L'"') { out.append(2*slashes+1,L'\\'); out.push_back(c); }
        else { out.append(slashes,L'\\'); out.push_back(c); }
        slashes=0;
    }
    out.append(slashes*2,L'\\'); out.push_back(L'"'); return out;
}
RunResult run(const fs::path& program,const Args& args,const Hooks& hooks,const Line& line) {
    hooks.check();
    std::wstring command=quote(program.wstring());
    for (auto& a:args) command+=L" "+quote(wide(a));
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    Handle reader,writer,input,process,thread;
    if (!CreatePipe(&reader.h,&writer.h,&sa,65536) || !SetHandleInformation(reader.h,HANDLE_FLAG_INHERIT,0))
        throw std::runtime_error("Windows could not open the FFmpeg progress pipe.");
    input.h=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
    if (input.h==INVALID_HANDLE_VALUE) throw std::runtime_error("Windows could not open the process input.");
    STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdInput=input.h; si.hStdOutput=writer.h; si.hStdError=writer.h;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))
        throw std::runtime_error("Could not start "+program.filename().u8string()+" (Windows error "+std::to_string(GetLastError())+").");
    process.h=pi.hProcess; thread.h=pi.hThread; writer.close(); input.close();
    Lines lines(line); std::array<char,32768> buffer{};
    try {
        for (;;) {
            hooks.check(); DWORD waiting=0, got=0;
            if (!PeekNamedPipe(reader.h,nullptr,0,nullptr,&waiting,nullptr)) break;
            if (waiting) {
                if (!ReadFile(reader.h,buffer.data(),std::min<DWORD>(waiting,(DWORD)buffer.size()),&got,nullptr)) break;
                lines.add(buffer.data(),got);
            } else if (WaitForSingleObject(process.h,30)==WAIT_OBJECT_0) {
                if (!PeekNamedPipe(reader.h,nullptr,0,nullptr,&waiting,nullptr) || !waiting) break;
            }
        }
        while (WaitForSingleObject(process.h,40)==WAIT_TIMEOUT) hooks.check();
        hooks.check(); DWORD code=1; GetExitCodeProcess(process.h,&code);
        return {(int)code,lines.finish()};
    } catch (...) {
        TerminateProcess(process.h,1); WaitForSingleObject(process.h,INFINITE); throw;
    }
}
#else
RunResult run(const fs::path& program,const Args& args,const Hooks& hooks,const Line& line) {
    hooks.check(); int fd[2]; if (pipe(fd)<0) throw std::runtime_error(std::strerror(errno));
    std::string prog=program.string(); std::vector<char*> argv{prog.data()};
    for (auto& s:args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    pid_t child=fork();
    if (child<0) { close(fd[0]); close(fd[1]); throw std::runtime_error("Could not start FFmpeg."); }
    if (child==0) {
        int input=open("/dev/null",O_RDONLY); dup2(input,STDIN_FILENO);
        dup2(fd[1],STDOUT_FILENO); dup2(fd[1],STDERR_FILENO);
        close(input); close(fd[0]); close(fd[1]); execv(prog.c_str(),argv.data()); _exit(127);
    }
    close(fd[1]); Lines lines(line); std::array<char,32768> buf{}; int status=0;
    try {
        pollfd p{fd[0],POLLIN,0};
        for (;;) {
            hooks.check(); int ready=poll(&p,1,60);
            if (ready<0 && errno==EINTR) continue;
            if (ready<0) throw std::runtime_error("Could not read FFmpeg progress.");
            if (!ready) continue;
            ssize_t n=read(fd[0],buf.data(),buf.size());
            if (n==0) break;
            if (n<0) { if (errno==EINTR) continue; throw std::runtime_error("Could not read FFmpeg output."); }
            lines.add(buf.data(),(size_t)n);
        }
        while (waitpid(child,&status,0)<0 && errno==EINTR) {}
    } catch (...) {
        kill(child,SIGKILL); while (waitpid(child,&status,0)<0 && errno==EINTR) {}
        close(fd[0]); throw;
    }
    close(fd[0]); hooks.check();
    return {WIFEXITED(status)?WEXITSTATUS(status):1,lines.finish()};
}
#endif
}
