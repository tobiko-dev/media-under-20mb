#include "media.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace drop {
namespace {
using std::string;
using std::vector;
using Error = std::runtime_error;
void add(Args& a, std::initializer_list<string> values) { a.insert(a.end(),values); }
bool starts(const string& s,const string& pre) { return s.compare(0,pre.size(),pre)==0; }
double number(const string& s,double fallback=0) {
    char* end=nullptr; double v=std::strtod(s.c_str(),&end);
    return end!=s.c_str() && *end==0 && std::isfinite(v) ? v : fallback;
}
double ratio(const string& s) {
    size_t slash=s.find('/'); if (slash==string::npos) return number(s);
    double denominator=number(s.substr(slash+1)); return denominator>0 ? number(s.substr(0,slash))/denominator : 0;
}
string unquote(string s) {
    if (s.size()<2 || s.front()!='"' || s.back()!='"') return s;
    string out;
    for (size_t i=1;i+1<s.size();++i) {
        if (s[i]=='\\' && i+2<s.size()) { ++i; if (s[i]=='n') { out+=' '; continue; } }
        out+=s[i];
    }
    return out;
}
struct Track {
    int index=-1, width=0,height=0,channels=0,rate=0,bits=0,rotation=0;
    bool cover=false, primary=false;
    string kind,codec,layout,timeBase,language,title;
    double fps=0, nominalFps=0, duration=0, first=INFINITY, last=-INFINITY;
    long long frames=0, packets=0, bytes=0, extraBytes=0;
    string filename;
    double span() const { return std::isfinite(last-first) && last>first ? last-first : duration; }
};
struct Media {
    std::map<int,Track> tracks;
    string format;
    double duration=0;
    vector<Track> of(const string& kind) const {
        vector<Track> v; for (auto& x:tracks) if (x.second.kind==kind && !x.second.cover) v.push_back(x.second); return v;
    }
};
string failure(const string& phase,const RunResult& run) {
    return phase+(run.diagnostic.empty() ? " (exit "+std::to_string(run.code)+")." : "\n"+run.diagnostic);
}
Media inspect(const fs::path& file,const Tools& tools,const Hooks& h,bool packets) {
    Media m;
    Args a={"-v","error","-protocol_whitelist","file","-show_entries",
        "format=format_name,duration:stream=index,codec_name,codec_type,width,height,channels,channel_layout,sample_rate,bits_per_raw_sample,bits_per_sample,avg_frame_rate,r_frame_rate,time_base,duration,nb_frames,extradata_size:stream_disposition=default,attached_pic,timed_thumbnails:stream_tags=language,title,filename:stream_side_data=rotation",
        "-of","flat","-i",file.u8string()};
    auto info=run(tools.ffprobe,a,h,[&](const string& line) {
        auto eq=line.find('='); if (eq==string::npos) return;
        string k=line.substr(0,eq), v=unquote(line.substr(eq+1));
        if (k=="format.format_name") { m.format=v; return; }
        if (k=="format.duration") { m.duration=number(v); return; }
        const string prefix="streams.stream.";
        if (!starts(k,prefix)) return;
        auto dot=k.find('.',prefix.size()); if (dot==string::npos) return;
        int pos=(int)number(k.substr(prefix.size(),dot-prefix.size()),-1); if (pos<0) return;
        auto& t=m.tracks[pos]; k=k.substr(dot+1);
        if (k=="index") t.index=(int)number(v,-1);
        else if (k=="codec_name") t.codec=v;
        else if (k=="codec_type") t.kind=v;
        else if (k=="width") t.width=(int)number(v);
        else if (k=="height") t.height=(int)number(v);
        else if (k=="channels") t.channels=(int)number(v);
        else if (k=="sample_rate") t.rate=(int)number(v);
        else if (k=="channel_layout") t.layout=v;
        else if (k=="time_base") t.timeBase=v;
        else if (k=="avg_frame_rate") t.fps=ratio(v);
        else if (k=="r_frame_rate") t.nominalFps=ratio(v);
        else if (k=="duration") t.duration=number(v);
        else if (k=="extradata_size") t.extraBytes=(long long)number(v);
        else if (k=="tags.filename") t.filename=v;
        else if (k=="nb_frames") t.frames=(long long)number(v);
        else if (k=="bits_per_raw_sample" || k=="bits_per_sample") t.bits=std::max(t.bits,(int)number(v));
        else if (k=="disposition.attached_pic" || k=="disposition.timed_thumbnails") t.cover |= v=="1";
        else if (k=="disposition.default") t.primary=v=="1";
        else if (k=="tags.language") t.language=v;
        else if (k=="tags.title") t.title=v;
        else if (k.size()>=9 && k.compare(k.size()-9,9,".rotation")==0) t.rotation=(int)number(v);
    });
    if (info.code) throw Error(failure("FFprobe could not read this media file.",info));
    // The flat writer's ordinal need not equal the absolute stream index.
    std::map<int,Track> indexed;
    for (auto& [pos,t]:m.tracks) if (t.index>=0) indexed[t.index]=t;
    m.tracks=std::move(indexed);
    if (m.of("video").empty() && m.of("audio").empty()) throw Error("The file contains no playable audio or video tracks.");
    if (!packets) return m;
    a={"-v","error","-protocol_whitelist","file","-show_packets","-show_entries",
       "packet=stream_index,pts_time,dts_time,duration_time,size","-of","compact=p=0:nk=0","-i",file.u8string()};
    auto scan=run(tools.ffprobe,a,h,[&](const string& line) {
        if (!starts(line,"stream_index=")) return;
        int id=-1; double pts=NAN,dts=NAN,len=0; long long bytes=0;
        for (size_t p=0;p<line.size();) {
            size_t q=line.find('|',p), e=line.find('=',p);
            if (e!=string::npos && (q==string::npos || e<q)) {
                const string k=line.substr(p,e-p), v=line.substr(e+1,q==string::npos ? q : q-e-1);
                if (k=="stream_index") id=(int)number(v,-1);
                else if (k=="pts_time") pts=number(v,NAN);
                else if (k=="dts_time") dts=number(v,NAN);
                else if (k=="duration_time") len=number(v);
                else if (k=="size") bytes=(long long)number(v);
            }
            if (q==string::npos) break;
            p=q+1;
        }
        auto it=m.tracks.find(id); if (it==m.tracks.end()) return;
        auto& t=it->second; ++t.packets; t.bytes+=bytes;
        double stamp=std::isfinite(pts) ? pts : dts;
        if (std::isfinite(stamp)) {
            t.first=std::min(t.first,stamp);
            t.last=std::max(t.last,stamp+len);
        }
    });
    if (scan.code) throw Error(failure("Could not read the complete packet timeline.",scan));
    return m;
}
struct Plan {
    vector<Track> video,audio,subs;
    vector<std::pair<int,int>> dimensions;
    vector<Track> attachments;
    bool fpsMode=true;
};
double origin(const Plan& p) {
    double t=INFINITY;
    for (auto* group:{&p.video,&p.audio,&p.subs}) for (auto& x:*group) t=std::min(t,x.first);
    return std::isfinite(t) ? t : 0;
}
double length(const Plan& p,double fallback) {
    double begin=origin(p), end=0;
    for (auto* group:{&p.video,&p.audio,&p.subs}) for (auto& t:*group) {
        if (std::isfinite(t.last)) end=std::max(end,t.last-begin);
        else end=std::max(end,t.duration);
    }
    return end>0 ? end : fallback;
}
bool matroska(const Plan& p) {
    if (!p.attachments.empty() || (p.video.empty() && !p.subs.empty())) return true;
    for (auto& t:p.subs) if (t.codec!="subrip" && t.codec!="srt" && t.codec!="mov_text" &&
        t.codec!="webvtt" && t.codec!="text") return true;
    return false;
}
string extension(const Plan& p) {
    if (matroska(p)) return p.video.empty()?".mka":".mkv";
    return p.video.empty()?".m4a":".mp4";
}
bool flacLossless(const Track& t) {
    return t.bits>0 && t.bits<=24 && (starts(t.codec,"pcm_s") || starts(t.codec,"pcm_u") ||
           t.codec=="alac" || t.codec=="flac");
}
struct Temp {
    fs::path dir;
    explicit Temp(const fs::path& parent) {
        auto seed=std::chrono::high_resolution_clock::now().time_since_epoch().count();
        for (int n=0;n<128;++n) {
            auto tryDir=parent/(".under20-work-"+std::to_string(seed)+"-"+std::to_string(n));
            std::error_code ec;
            if (fs::create_directory(tryDir,ec)) { dir=tryDir; return; }
            if (ec && ec!=std::errc::file_exists) throw Error("Cannot write beside this file. "+ec.message());
        }
        throw Error("Could not create a unique working folder beside the file.");
    }
    ~Temp() { std::error_code ignored; fs::remove_all(dir,ignored); }
};
fs::path publish(const fs::path& source,const fs::path& tmp,const Hooks& h) {
    const string stem=source.stem().u8string()+"-under-20MB";
    for (unsigned int n=1;n<1000000;++n) {
        h.check(); auto name=stem+(n==1 ? "" : " ("+std::to_string(n)+")")+tmp.extension().u8string();
        auto out=source.parent_path()/fs::u8path(name);
#ifdef _WIN32
        if (MoveFileExW(tmp.c_str(),out.c_str(),MOVEFILE_WRITE_THROUGH)) return out;
        auto e=GetLastError();
        if (e==ERROR_ALREADY_EXISTS || e==ERROR_FILE_EXISTS || (e==ERROR_ACCESS_DENIED && fs::exists(out))) continue;
        throw Error("Could not save the finished copy beside the original (Windows error "+std::to_string(e)+").");
#else
        if (::link(tmp.c_str(),out.c_str())==0) return out;
        if (errno==EEXIST) continue;
        throw Error("Could not publish the finished copy beside the original.");
#endif
    }
    throw Error("There are too many copies with this name.");
}
Args input(const fs::path& file,bool strict=false) {
    Args a={"-hide_banner","-nostdin","-y","-v","error","-xerror","-progress","pipe:1","-nostats",
        "-protocol_whitelist","file","-noautorotate"};
    if (strict) add(a,{"-err_detect","explode"});
    add(a,{"-i",file.u8string()}); return a;
}
void mapTracks(Args& a,const vector<Track>& tracks) {
    for (auto& t:tracks) add(a,{"-map","0:"+std::to_string(t.index)});
}
void maps(Args& a,const Plan& p) {
    mapTracks(a,p.video); mapTracks(a,p.audio); mapTracks(a,p.subs);
    if (!p.attachments.empty()) { mapTracks(a,p.attachments); add(a,{"-c:t","copy"}); }
}
void container(Args& a,const Plan& p,const fs::path& path) {
    // Leave pictures and arbitrary container payloads out of ordinary share copies.
    // Stream language, title and default/forced flags travel with the explicit mappings.
    if (!matroska(p)) add(a,{"-movflags","+faststart"});
    for (size_t i=0;i<p.subs.size();++i) {
        string codec=matroska(p) ? (p.subs[i].codec=="mov_text" ? "srt" : "copy") : "mov_text";
        add(a,{"-c:s:"+std::to_string(i),codec});
    }
    add(a,{"-map_metadata","-1"});
    for (size_t i=0;i<p.attachments.size();++i)
        add(a,{"-map_metadata:s:t:"+std::to_string(i),"0:s:"+std::to_string(p.attachments[i].index)});
    const std::pair<const vector<Track>*,const char*> groups[]={{&p.audio,"a"},{&p.video,"v"},{&p.subs,"s"}};
    for (const auto& group:groups) {
        for (size_t i=0;i<group.first->size();++i) {
            auto& t=group.first->at(i); string key="-metadata:s:"+string(group.second)+":"+std::to_string(i);
            if (!t.language.empty()) add(a,{key,"language="+t.language});
            if (!t.title.empty()) add(a,{key,"title="+t.title});
        }
    }
    a.push_back(path.u8string());
}
void transcode(const Tools& tools,const Args& args,const Hooks& h,const string& phase,double duration,int start,int end) {
    h.progress(phase,start); int sent=start;
    auto r=run(tools.ffmpeg,args,h,[&](const string& s) {
        if (!starts(s,"out_time_us=") || !(duration>0)) return;
        double progress=std::clamp(number(s.substr(12))/1000000.0/duration,0.0,1.0);
        int pc=start+(int)((end-start)*progress);
        if (pc>sent) { sent=pc; h.progress(phase,pc); }
    });
    if (r.code) throw Error(failure(phase+" failed.",r));
}
std::string trackLabel(const Track& t) {
    string x=t.kind+" #"+std::to_string(t.index);
    if (!t.language.empty() && t.language!="und") x+=" ["+t.language+"]";
    if (!t.title.empty()) x+=" "+t.title;
    return x+" ("+t.codec+")";
}
void verify(const fs::path& file,const Plan& plan,const Tools& tools,const Hooks& h,double duration) {
    h.progress("Checking tracks and timing",86);
    Media out=inspect(file,tools,h,true);
    Plan actual{out.of("video"),out.of("audio"),out.of("subtitle"),{},out.of("attachment")};
    double delta=origin(actual)-origin(plan);
    auto check=[&](const vector<Track>& src,const vector<Track>& dst) {
        if (src.size()!=dst.size()) throw Error("Verification found an unexpected track count.");
        for (size_t i=0;i<src.size();++i) {
            auto &a=src[i], &b=dst[i];
            if (a.packets && !b.packets) throw Error("Verification found an empty "+a.kind+" track.");
            if (a.kind=="subtitle" || a.kind=="attachment") continue;
            if (a.kind=="audio" && a.channels!=b.channels) throw Error("Verification found a change in audio channel count.");
            double tol=a.kind=="audio" ? std::max(0.12,2304.0/std::max(8000,std::min(a.rate,b.rate))) :
                std::max(.045,1.1/std::max(1.0,a.fps>0?a.fps:a.nominalFps));
            if (a.kind=="video") {
                if (b.width!=plan.dimensions[i].first || b.height!=plan.dimensions[i].second || ((b.rotation-a.rotation)%360)!=0)
                    throw Error("Verification found an unexpected change in video dimensions or rotation.");
                if (a.frames && b.frames && a.frames!=b.frames)
                    throw Error("Video frame counts did not match.");
                if ((a.codec=="h264" || a.codec=="hevc") && (b.codec=="h264" || b.codec=="hevc") &&
                    a.packets>0 && a.packets!=b.packets && (!a.frames || !b.frames))
                    throw Error("Video sample counts did not match.");
            }
            if (a.span()>0 && b.span()>0 && std::abs(a.span()-b.span())>tol)
                throw Error("Verification found a change in the duration of "+trackLabel(a)+".");
            if (std::isfinite(a.first) && std::isfinite(b.first) && std::abs(b.first-a.first-delta)>tol)
                throw Error("Verification found a change in track synchronization.");
        }
    };
    check(plan.video,actual.video); check(plan.audio,actual.audio); check(plan.subs,actual.subs);
    check(plan.attachments,actual.attachments);
    Args a=input(file,true); mapTracks(a,actual.video); mapTracks(a,actual.audio);
    add(a,{"-c:v","wrapped_avframe","-c:a","pcm_s16le","-f","null","-"});
    transcode(tools,a,h,"Decoding the finished copy",duration,89,99);
}
void consent(const Hooks& h,const string& request) {
    h.check(); bool yes=h.approve(request); h.check(); if (!yes) throw Declined();
}
enum class Trim { Audio, Subtitles, Both };
bool removeExtras(Plan& p,Trim which,const Hooks& h,const string& why) {
    vector<Track> keep=p.audio, removed;
    if (which!=Trim::Subtitles && p.audio.size()>1) {
        auto main=std::find_if(p.audio.begin(),p.audio.end(),[](auto& t){return t.primary;});
        if (main==p.audio.end()) main=p.audio.begin();
        keep={*main}; for (auto& t:p.audio) if (t.index!=main->index) removed.push_back(t);
    }
    if (which!=Trim::Audio) removed.insert(removed.end(),p.subs.begin(),p.subs.end());
    if (removed.empty()) return false;
    string msg=why+"\n\nRemove these tracks from the new copy?\n";
    for (auto& t:removed) msg+="\n"+trackLabel(t);
    if (!keep.empty()) msg+="\n\nAudio kept: "+trackLabel(keep.front());
    consent(h,msg+"\n\nChoose No to skip this file.");
    p.audio=keep; if (which!=Trim::Audio) p.subs.clear();
    return true;
}
bool reduceResolution(Plan& p,const Hooks& h) {
    auto proposed=p.dimensions; string changes;
    for (size_t i=0;i<proposed.size();++i) {
        auto [w,ht]=proposed[i]; const int longest=std::max(w,ht);
        for (int edge:{1920,1280,960,640,480,320}) {
            if (edge>=longest) continue;
            auto dim=[&](int original) { return std::max(2,2*(int)std::round(original*(double)edge/longest/2)); };
            proposed[i]={dim(w),dim(ht)};
            changes+="\n"+trackLabel(p.video[i])+": "+std::to_string(w)+" × "+std::to_string(ht)+" → "+
                std::to_string(proposed[i].first)+" × "+std::to_string(proposed[i].second);
            break;
        }
    }
    if (changes.empty()) return false;
    consent(h,"Full-size encoding did not fit the limit. Use these dimensions for the new copy?\n"+changes+
        "\n\nDuration, frame timing and audio channels will be preserved. Choose No to skip this file.");
    p.dimensions=std::move(proposed); return true;
}
long long attachmentBytes(const Plan& p) {
    long long n=0; for (auto& t:p.attachments) n+=t.extraBytes; return n;
}
void omitAttachments(Plan& p,const Hooks& h,const string& why) {
    if (p.attachments.empty()) return;
    string msg=why+"\n\nLeave these attachments out of the new copy?";
    for (auto& t:p.attachments) msg+="\n"+(t.filename.empty()?trackLabel(t):t.filename);
    consent(h,msg+"\n\nRemoving an embedded font can change subtitle appearance. Choose No to skip this file.");
    p.attachments.clear();
}
struct Rates {
    vector<int> video, audio;
    double available=0;
    bool tight=false;
};
Rates ratesFor(const Plan& p,double duration,double scale) {
    long long subtitleBytes=0;
    for (auto& t:p.subs) subtitleBytes+=t.bytes;
    // Reserve fixed headers, mapped subtitle payloads and sample-table entries.
    double packetOverhead=0;
    for (auto& v:p.video) packetOverhead+=12.0*v.packets;
    for (auto& a:p.audio) packetOverhead+=8*std::max(1,a.rate)*std::max(0.0,a.span())/1024.0;
    Rates r;
    r.available=((limit-48000.0)*.978-subtitleBytes-attachmentBytes(p)-packetOverhead)*8.0/duration*scale;
    vector<double> goals, minimums;
    for (auto& a:p.audio) {
        double channels=std::max(1,a.channels);
        goals.push_back(std::min(768000.0,channels*(p.video.empty()?160000.0:112000.0)));
        minimums.push_back(std::max(8000.0,channels*6000.0));
    }
    double ideal=std::accumulate(goals.begin(),goals.end(),0.0);
    double amin=std::accumulate(minimums.begin(),minimums.end(),0.0);
    vector<double> videoMin;
    for (auto [w,ht]:p.dimensions) videoMin.push_back(std::clamp(8*std::sqrt((double)w*ht),2500.0,12000.0));
    double vmin=std::accumulate(videoMin.begin(),videoMin.end(),0.0);
    if (r.available<amin+vmin) { r.tight=true; return r; }
    double audioBudget=std::min(ideal,p.video.empty()?r.available:std::max(amin,std::min(r.available-vmin,r.available*.23)));
    for (size_t i=0;i<goals.size();++i) {
        double spare=std::max(0.0,audioBudget-amin);
        r.audio.push_back((int)std::floor(minimums[i]+spare*(goals[i]-minimums[i])/std::max(1.0,ideal-amin)));
    }
    double remaining=r.available-std::accumulate(r.audio.begin(),r.audio.end(),0.0);
    vector<double> weights; for (size_t i=0;i<p.video.size();++i) {
        auto [w,ht]=p.dimensions[i]; weights.push_back(std::sqrt((double)w*ht*std::max(1.0,p.video[i].fps)));
    }
    auto sum=std::accumulate(weights.begin(),weights.end(),0.0);
    for (size_t i=0;i<weights.size();++i)
        r.video.push_back((int)std::clamp(videoMin[i]+(remaining-vmin)*weights[i]/sum,2500.0,180000000.0));
    return r;
}
bool hasFpsMode(const Tools& tools,const Hooks& h) {
    bool found=false;
    auto help=run(tools.ffmpeg,{"-hide_banner","-h","full"},h,[&](const string& line) {
        if (starts(line,"-fps_mode")) found=true;
    });
    if (help.code) throw Error(failure("Could not identify the installed FFmpeg features.",help));
    return found;
}
void videoArgs(Args& a,const Plan& p,const Rates& r,const Temp& temp,int pass) {
    if (!p.fpsMode) add(a,{"-vsync","0"});
    for (size_t i=0;i<p.video.size();++i) {
        const auto& v=p.video[i]; auto [w,ht]=p.dimensions[i]; auto n=":v:"+std::to_string(i);
        add(a,{"-c"+n,"libx264","-preset"+n,"fast","-b"+n,std::to_string(r.video[i]),
            "-pix_fmt"+n,(w%2 || ht%2)?"yuv444p":"yuv420p",
            "-threads"+n,std::to_string(std::max(2,6/(int)p.video.size())),
            "-pass"+n,std::to_string(pass),"-passlogfile"+n,(temp.dir/("pass-"+std::to_string(i))).u8string()});
        if (p.fpsMode) add(a,{"-fps_mode"+n,"passthrough"});
        if (ratio(v.timeBase)>0) add(a,{"-enc_time_base"+n,v.timeBase});
        if (w!=v.width || ht!=v.height)
            add(a,{"-filter"+n,"scale="+std::to_string(w)+":"+std::to_string(ht)+":flags=lanczos"});
    }
}
void audioArgs(Args& a,const Plan& p,const Rates& r) {
    for (size_t i=0;i<p.audio.size();++i) {
        const auto& t=p.audio[i]; auto n=":a:"+std::to_string(i);
        int target=r.audio[i]; double original=t.span()>0?t.bytes*8.0/t.span():0;
        if (t.codec=="aac" && original>0 && original<=target) { add(a,{"-c"+n,"copy"}); continue; }
        add(a,{"-c"+n,"aac","-b"+n,std::to_string(target)});
        if (t.channels>0) add(a,{"-ac"+n,std::to_string(t.channels)});
        int perChannel=target/std::max(1,t.channels);
        int ceiling=perChannel<10000?16000:perChannel<16000?24000:perChannel<24000?32000:96000;
        int nearest=0;
        for (int hz:{96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350})
            if (hz<=ceiling && hz<=t.rate) { nearest=hz; break; }
        if (nearest) add(a,{"-ar"+n,std::to_string(nearest)});
    }
}
void encode(const fs::path& src,const fs::path& out,const Plan& p,const Rates& r,const Temp& temp,
            const Tools& tools,const Hooks& h,double duration,int attempt) {
    string label=attempt==1?"":" · size retry "+std::to_string(attempt-1);
    if (!p.video.empty()) {
        Args a=input(src); mapTracks(a,p.video); videoArgs(a,p,r,temp,1);
        add(a,{"-an","-sn","-dn","-f","null","-"});
        transcode(tools,a,h,"Analyzing video · pass 1 of 2"+label,duration,7,37);
    }
    Args a=input(src); maps(a,p);
    if (!p.video.empty()) videoArgs(a,p,r,temp,2);
    audioArgs(a,p,r); container(a,p,out);
    string phase=p.video.empty()?"Encoding AAC":"Encoding video · pass 2 of 2";
    transcode(tools,a,h,phase+label,duration,p.video.empty()?8:37,85);
}
bool canRewrap(const Plan& p) {
    double payload=0;
    for (auto& t:p.video) { if (t.codec!="h264") return false; payload+=t.bytes; }
    for (auto& t:p.audio) { if (t.codec!="aac") return false; payload+=t.bytes; }
    for (auto& t:p.subs) payload+=t.bytes;
    return payload>0 && payload<limit;
}
struct PCM {string codec; int bytes,hz;};
void wavCopy(const fs::path& src,const fs::path& out,const Track& t,const Tools& tools,const Hooks& h,double duration) {
    const int channels=std::max(1,t.channels), rate=t.rate;
    if (rate<=0) throw Error("The source has no usable sample rate.");
    vector<PCM> choices;
    // Preserve the original PCM representation whenever the sample data fits.
    if (starts(t.codec,"pcm_") && (t.codec=="pcm_u8" || t.codec.find("le")!=string::npos) && t.bits>0)
        choices.push_back({t.codec,(t.bits+7)/8,rate});
    vector<int> rates{rate};
    for (int hz:{192000,176400,96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000})
        if (hz<rate) rates.push_back(hz);
    for (int hz:rates) {
        if (t.bits>16 || t.bits==0) choices.push_back({"pcm_s24le",3,hz});
        choices.push_back({"pcm_s16le",2,hz});
    }
    for (int hz:rates) choices.push_back({"pcm_u8",1,hz});
    double seconds=t.span()>0?t.span():duration;
    for (auto& c:choices) {
        if (std::ceil(seconds*c.hz)*channels*c.bytes+256>=limit) continue;
        Args a=input(src); add(a,{"-map","0:"+std::to_string(t.index),"-map_metadata","-1","-c:a",c.codec,
            "-ar",std::to_string(c.hz),"-ac",std::to_string(channels),out.u8string()});
        transcode(tools,a,h,"Writing PCM WAV · "+std::to_string(c.hz)+" Hz",duration,8,85);
        if (fs::file_size(out)<limit) return;
    }
    throw Error("A full-length PCM WAV cannot fit at the available sample rates and channel count. Clear Keep WAV and drop this file again to use audio compression.");
}
} // namespace

std::string sizeText(std::uintmax_t n) {
    std::ostringstream o; o<<std::fixed<<std::setprecision(2)<<n/1000000.0<<" MB"; return o.str();
}
Result compress(const fs::path& incoming,bool keepWav,const Tools& tools,const Hooks& h) {
    Result result;
    try {
        h.check(); fs::path src=fs::absolute(incoming);
        if (!fs::is_regular_file(src)) throw Error("Choose a local media file. Folders cannot be queued.");
        result.before=fs::file_size(src); bool small=result.before<limit;
        h.progress("Reading media tracks",2);
        Media media=inspect(src,tools,h,!small);
        if (small) {
            result.state=Result::Skipped; result.path=src; result.message="Already under 20 MB"; return result;
        }
        Plan p{media.of("video"),media.of("audio"),media.of("subtitle"),{},media.of("attachment")};
        for (auto& v:p.video) p.dimensions.emplace_back(v.width,v.height);
        Temp temp(src.parent_path());
        auto duration=[&](){
            double d=length(p,media.duration);
            if (!(d>0) || !std::isfinite(d)) throw Error("A reliable duration could not be measured for this file.");
            return d;
        };
        auto accept=[&](const fs::path& candidate,const string& note) {
            auto size=fs::file_size(candidate); if (size==0 || size>=limit) return false;
            verify(candidate,p,tools,h,duration());
            h.check(); auto verifiedSize=fs::file_size(candidate);
            if (verifiedSize!=size || verifiedSize>=limit) throw Error("The copy changed during verification.");
            result.path=publish(src,candidate,h);
            result.state=Result::Saved; result.after=size; result.message=note; return true;
        };
        if (keepWav && p.video.empty()) {
            removeExtras(p,Trim::Both,h,"PCM WAV can store one audio track.");
            omitAttachments(p,h,"PCM WAV cannot store embedded file attachments.");
            auto path=temp.dir/"copy.wav"; wavCopy(src,path,p.audio.at(0),tools,h,duration());
            if (!accept(path,"Verified · PCM WAV")) throw Error("The PCM WAV did not fit.");
            return result;
        }
        if (p.video.empty() && p.audio.size()==1 && p.subs.empty() && p.attachments.empty() && flacLossless(p.audio.front())) {
            auto path=temp.dir/"copy.flac"; Args a=input(src); mapTracks(a,p.audio);
            add(a,{"-map_metadata","-1","-c:a","flac","-compression_level","8",path.u8string()});
            bool encoded=false;
            try { transcode(tools,a,h,"Trying lossless FLAC",duration(),7,80); encoded=true; }
            catch (const Error&) { h.check(); } // The installed encoder may reject an unusual layout.
            if (encoded && accept(path,"Verified · lossless FLAC")) return result;
            std::error_code ignored; fs::remove(path,ignored);
        }
        if (canRewrap(p)) {
            auto path=temp.dir/fs::u8path("rewrap"+extension(p));
            Args a=input(src); maps(a,p); add(a,{"-c","copy"}); container(a,p,path);
            // Some source containers have legal packets that cannot be stream-copied into MP4.
            try {
                transcode(tools,a,h,"Trying original audio and video streams",duration(),7,77);
                if (accept(path,"Verified · original audio/video streams")) return result;
            } catch (const Error&) { /* Re-encoding starts from the original. */ }
            std::error_code ignored; fs::remove(path,ignored);
        }
        if (!p.video.empty()) p.fpsMode=hasFpsMode(tools,h);
        for (;;) {
            double d=duration(), scale=1;
            auto ext=extension(p); auto path=temp.dir/fs::u8path("copy"+ext);
            bool tight=false;
            for (int attempt=1;attempt<=4;++attempt) {
                h.check(); Rates r=ratesFor(p,d,scale);
                if (r.tight) { tight=true; break; }
                encode(src,path,p,r,temp,tools,h,d,attempt);
                string detail=ext==".mp4"?(p.audio.empty()?"H.264 MP4":"H.264 / AAC MP4"):
                    ext==".m4a"?"AAC M4A":"Matroska · retained tracks";
                if (accept(path,"Verified · "+detail)) return result;
                auto observed=fs::file_size(path);
                if (observed==0) throw Error("FFmpeg produced an empty file.");
                scale*=std::min(.88,.955*(double)limit/observed);
                fs::remove(path);
            }
            long long subtitleBytes=0; for (auto& s:p.subs) subtitleBytes+=s.bytes;
            if (!p.attachments.empty() && attachmentBytes(p)>limit*.2) {
                omitAttachments(p,h,"Embedded attachments leave too little space for the media."); continue;
            }
            if (!p.subs.empty() && (subtitleBytes>limit*.2 || (tight && p.audio.size()<=1)))
                { removeExtras(p,Trim::Subtitles,h,"The subtitle payload leaves too little space for the media."); continue; }
            if (p.audio.size()>1 && (tight || p.video.empty())) {
                removeExtras(p,Trim::Audio,h,"The full set of audio tracks would exceed the available bitrate."); continue;
            }
            double minAudio=0; for (auto& a:p.audio) minAudio+=std::max(8000.0,6000.0*a.channels);
            bool smallerCouldFit=ratesFor(p,d,1).available>minAudio+2500*p.video.size();
            if (!p.video.empty() && (!tight || smallerCouldFit) && reduceResolution(p,h)) continue;
            if (removeExtras(p,Trim::Both,h,"The copy still exceeds the size limit with all tracks included.")) continue;
            throw Error("A full-length copy could not fit under 20,000,000 bytes with the retained streams.");
        }
    } catch (const Cancelled&) { result.state=Result::Canceled; result.message="Canceled"; }
      catch (const Declined&) { result.state=Result::Skipped; result.message="Skipped · proposed change declined"; }
      catch (const std::exception& e) { result.state=Result::Failed; result.message=e.what(); }
    return result;
}
}
