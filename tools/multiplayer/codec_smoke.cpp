// Standalone codec verification; no simulation engine or Meson required.
// g++ -std=c++20 -O2 -ffunction-sections -fdata-sections -DTPT_MP_WEBP=1 \
//   -Isrc src/multiplayer/Canvas.cpp tools/multiplayer/codec_smoke.cpp \
//   -Wl,--gc-sections -lz -lwebp -o /tmp/tpt-codec-smoke
#include "multiplayer/Canvas.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <chrono>
using namespace multiplayer;
int main()
{
    try
    {
        if (!SupportsWebP()) throw std::runtime_error("WebP support required");
        std::string colors(CanvasBytes,'\0');
        uint32_t random=17423;
        auto noise=[&]() {random ^= random<<13;random^=random>>17;random^=random<<5;return random;};
        // Entropic flame/lava scene: difficult for XOR + zlib, compressible as an image.
        for (int y=0;y<YRES;++y) for (int x=0;x<XRES;++x)
        {
            size_t i=size_t(y)*XRES+x;
            int n=int(noise()%256);
            bool active = x>=75 && x<540 && y>=30 && y<355;
            colors[i]=char(active?n:0);
            colors[CanvasPixels+i]=char(active?(n/2 + x/8)%256:0);
            colors[2*CanvasPixels+i]=char(active?(n/3+y/8)%256:0);
        }
        for (int scale : {1,2,3})
        {
            auto now=std::chrono::steady_clock::now();
            CanvasFrame frame{1,100,7,0,false,PackCanvas(colors,40,scale)};
            double encodeMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-now).count();
            auto lossless=EncodeCanvas(frame,1);
            auto low=EncodeCanvas(frame,1,nullptr,true);
            if (low[4]!=3) throw std::runtime_error("WebP not selected");
            auto decoded=DecodeCanvas(low);
            if (decoded.frame.pixels->planes.size()!=CanvasBytes || !decoded.frame.lossy)
                throw std::runtime_error("incorrect decoded size/metadata");
            auto nextPacket=EncodeCanvas(frame,2,&decoded,true);
            if (nextPacket[4]!=3 && nextPacket[4]!=0)
                throw std::runtime_error("unsafe delta sent against lossy baseline");
            auto next=DecodeCanvas(nextPacket,&decoded);
            if (next.frame.pixels->planes!=decoded.frame.pixels->planes)
                throw std::runtime_error("drifting independent frame");
            auto invalid=low;invalid[low.size()-1]^=1;
            bool failed=false;try {DecodeCanvas(invalid);}catch (...) {failed=true;}
            if (!failed) throw std::runtime_error("corruption was accepted");
            double mse=0;
            for (size_t i=0;i<CanvasBytes;++i)
            {
                int d=int(uint8_t(colors[i]))-int(uint8_t(decoded.frame.pixels->planes[i]));
                mse+=d*d;
            }
            mse/=CanvasBytes;
            std::cout << "scale=" << scale << ": WebP keyframe " << low.size()
                << " bytes vs original lossless " << lossless.size()
                << ", reduction " << 100.0*(1.0-double(low.size())/lossless.size())
                << "%, encode " << encodeMs << " ms, PSNR " << 10*std::log10(255.0*255.0/mse)
                << " dB, roundtrip and corruption tests PASS\n";
        }
        return 0;
    }
    catch(const std::exception &e) {std::cerr << e.what() << '\n';return 1;}
}
