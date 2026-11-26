#include "media.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>
int main(int argc,char** argv) {
    if (argc<2) return 2;
    bool wav=false,yes=false; int failures=0;
    std::atomic_bool canceled{false};
    if (auto t=std::getenv("CANCEL_AFTER_MS"))
        std::thread([&,n=std::atoi(t)]{std::this_thread::sleep_for(std::chrono::milliseconds(n));canceled=true;}).detach();
    const char* enc=std::getenv("TEST_FFMPEG"),*probe=std::getenv("TEST_FFPROBE");
    drop::Tools tools{enc?enc:"/usr/bin/ffmpeg",probe?probe:"/usr/bin/ffprobe"};
    for (int i=1;i<argc;++i) {
        std::string arg=argv[i];
        if (arg=="--wav") { wav=true; continue; }
        if (arg=="--yes") { yes=true; continue; }
        std::string stage;
        drop::Hooks hooks{canceled,[&](auto s,int p){if (s!=stage){std::cerr<<p<<"% "<<s<<"\n";stage=s;}},
            [&](auto s){std::cerr<<"APPROVAL "<<s<<"\n"; return yes;}};
        auto r=drop::compress(drop::fs::u8path(arg),wav,tools,hooks);
        std::cout<<r.state<<"\t"<<r.before<<"\t"<<r.after<<"\t"<<r.path.u8string()<<"\t"<<r.message<<"\n"<<std::flush;
        failures+=r.state==drop::Result::Failed;
    }
    return failures?1:0;
}
