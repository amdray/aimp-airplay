#include "album_art.h"
#include "com_ptr.h"
#include <algorithm>

namespace airplay::aimp {
namespace {
void PrepareCover(IAIMPImage* image, IAIMPImageContainer* container, CoverRequest& result) {
    try {
        ComPtr<IAIMPImage> source(image), copy;
        if (!source && container) container->CreateImage(source.put());
        if (!source || FAILED(source->Clone(copy.put())) || !copy) return;
        SIZE size{};
        if (FAILED(copy->GetSize(&size)) || size.cx <= 0 || size.cy <= 0) return;
        if (std::max(size.cx, size.cy) > 512) {
            double scale = 512.0 / std::max(size.cx, size.cy);
            if (FAILED(copy->Resize(std::max(1, int(size.cx * scale)), std::max(1, int(size.cy * scale))))) return;
        }
        ComPtr<IAIMPMemoryStream> stream;
        if (FAILED(result.core->CreateObject(IID_IAIMPMemoryStream, stream.out())) ||
            FAILED(copy->SaveToStream(stream.get(), AIMP_IMAGE_FORMAT_JPG))) return;
        auto length = stream->GetSize();
        if (length > 0 && length <= 2 * 1024 * 1024 && stream->GetData()) {
            auto* bytes = static_cast<uint8_t*>(stream->GetData());
            result.bytes.assign(bytes, bytes + size_t(length));
        }
    } catch (...) {}
}
}
void CALLBACK ReceiveCoverAsync(IAIMPImage* image, IAIMPImageContainer* container, void* user) {
    auto* request = static_cast<CoverRequest*>(user);
    PrepareCover(image, container, *request);
    request->completed.store(true, std::memory_order_release);
}
}
