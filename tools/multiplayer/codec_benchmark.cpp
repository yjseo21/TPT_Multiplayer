// Compare lossless and WebP packets on real raw rendered TPT image captures.
// Build command in tools/multiplayer/README-CODEC.md.
#include "multiplayer/Canvas.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>
using namespace multiplayer;
using Clock=std::chrono::steady_clock;
int main(int argc,char**argv)
{
    if (argc<2 || !SupportsWebP()) {std::cerr<<"Usage: codec-benchmark file.raw ... (compile with WebP)\n";return 1;}
    std::cout<<"scene,quality,scale,frames,lossless_mbps_20fps,webp_mbps_20fps,reduction_pct,psnr_db,encode_ms,decode_ms\n";
    for(int file=1;file<argc;++file) for(int quality: {30,50,70}) for(int scale: {1,2,3})
    {
        std::ifstream in(argv[file],std::ios::binary);
        if(!in) {std::cerr<<"Cannot open "<<argv[file]<<"\n";return 1;}
        size_t n=0,losslessBytes=0,webpBytes=0;
        double mse=0,encodeMs=0,decodeMs=0;
        ReceivedCanvas losslessBaseline, webpBaseline;
        std::string raw(CanvasBytes,'\0');
        while(in.read(raw.data(),raw.size()))
        {
            auto begin=Clock::now();
            CanvasFrame f{uint64_t(n+1),uint64_t(n*3),0,0,false,PackCanvas(raw,quality,scale)};
            auto originalPacket=EncodeCanvas(f,n+1,losslessBaseline.id ? &losslessBaseline:nullptr);
            auto packet=EncodeCanvas(f,n+1,webpBaseline.id ? &webpBaseline:nullptr,true);
            encodeMs+=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
            begin=Clock::now();
            auto orig=DecodeCanvas(originalPacket, losslessBaseline.id ? &losslessBaseline:nullptr);
            auto dec=DecodeCanvas(packet,webpBaseline.id ? &webpBaseline:nullptr);
            decodeMs+=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
            if(orig.frame.pixels->planes!=raw) {std::cerr<<"Lossless mismatch\n";return 2;}
            for(size_t i=0;i<CanvasBytes;++i) {int d=int(uint8_t(raw[i]))-int(uint8_t(dec.frame.pixels->planes[i]));mse+=d*d;}
            losslessBaseline={uint64_t(n+1),f};
            webpBaseline={uint64_t(n+1),f};
            if(packet[4]==3) webpBaseline.frame.lossy=true;
            losslessBytes+=originalPacket.size();webpBytes+=packet.size();++n;
        }
        if(!n) {std::cerr<<"Empty/incomplete capture\n";return 1;}
        double b=20*8./(n*1e6);
        std::cout<<argv[file]<<','<<quality<<','<<scale<<','<<n<<','<<std::fixed<<std::setprecision(3)
                 <<losslessBytes*b<<','<<webpBytes*b<<','<<100*(1.-double(webpBytes)/losslessBytes)<<','
                 <<(mse?10*log10(255.*255.*CanvasBytes*n/mse):100)<<','<<encodeMs/n<<','<<decodeMs/n<<'\n';
    }
}
