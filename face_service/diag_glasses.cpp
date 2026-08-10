// Glasses-mode alignment diagnostic.
//
// Loads the same ONNX stack the service uses (SCRFD detector + 2d106det
// landmarks + w600k_mbf recognizer) and measures how "wearing glasses"
// affects the match distance under two alignment modes:
//   - AlignMode::OuterEye (historical: outer eye corners 39/93)
//   - AlignMode::EyeCenter (glasses-robust candidate: eye centers 38/88)
//
// Usage: diag_glasses.exe <image-directory>
// The directory should contain paired JPEGs: "<name> 不戴眼镜.jpg" and
// "<name> 戴眼镜.jpg". The tool pairs them by the part before the suffix.
//
// Output: a distance table. Lower distance = more similar. The "跨眼镜"
// (cross-glasses) distance is the key number — if EyeCenter alignment pulls it
// meaningfully below OuterEye, glasses-mode alignment is worth pursuing.

#include "onnx_models.h"
#include "landmark_detector.h"
#include "../common/logger.h"

#include <dlib/image_loader/load_image.h>
#include <dlib/matrix.h>
#include <dlib/pixel.h>
#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <windows.h>

namespace fs = std::filesystem;

static std::string baseName(const fs::path& p) {
    // Work in wide strings: path.wstring() is code-page-agnostic, whereas
    // path.string() on MSVC returns local-codepage (GBK) bytes that don't
    // match the UTF-8 literals below.
    std::wstring n = p.stem().wstring();
    const wchar_t* suf[] = {L" 不戴眼镜", L" 戴眼镜", L"不戴眼镜", L"戴眼镜"};
    for (const wchar_t* s : suf) {
        size_t sl = wcslen(s);
        if (n.size() >= sl && n.compare(n.size() - sl, sl, s) == 0) {
            n = n.substr(0, n.size() - sl);
            break;
        }
    }
    // Convert back to UTF-8 for internal keys (output labels may be garbled
    // in a GBK console but the distances are what matter).
    int len = WideCharToMultiByte(CP_UTF8, 0, n.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, n.c_str(), -1, &out[0], len, nullptr, nullptr);
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: diag_glasses.exe <image-directory>\n";
        return 1;
    }
    std::string dirStr = argv[1];
    std::wstring modelsDir = L"D:\\Fold\\FaceLogin\\models";

    // --- Load models (same as the service) ---
    facelogin::OnnxDetector detector;
    if (!detector.Initialize(modelsDir + L"\\det_500m.onnx")) {
        std::cout << "FAILED to load SCRFD detector\n";
        return 1;
    }
    facelogin::OnnxLandmarkDetector landmark;
    if (!landmark.Initialize(modelsDir + L"\\2d106det.onnx")) {
        std::cout << "FAILED to load 2d106det\n";
        return 1;
    }
    facelogin::OnnxRecognizer recognizer;
    if (!recognizer.Initialize(modelsDir + L"\\w600k_mbf.onnx")) {
        std::cout << "FAILED to load w600k_mbf\n";
        return 1;
    }

    // --- Load images, group by subject ---
    std::map<std::string, std::map<std::string, fs::path>> subjects; // subject -> {glasses?, path}
    for (const auto& entry : fs::directory_iterator(dirStr)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(c));
        if (ext != ".jpg" && ext != ".jpeg" && ext != ".png") continue;
        std::wstring wname = entry.path().filename().wstring();
        // "不戴眼镜" also contains "戴眼镜" as a substring — check the standalone
        // marker, not a bare find. Wearing glasses iff it says 戴眼镜 and does NOT
        // start with 不戴眼镜 (i.e. the negated form).
        bool glasses = (wname.find(L"戴眼镜") != std::wstring::npos &&
                        wname.find(L"不戴眼镜") == std::wstring::npos);
        std::string subj = baseName(entry.path());
        std::cout << "  [collect] glasses=" << (glasses ? "1" : "0") << " subj='" << subj << "' file=" << entry.path().filename().string() << "\n";
        subjects[subj][glasses ? "glasses" : "plain"] = entry.path();
    }

    // --- Compute embeddings for every image under both alignments ---
    struct ImgEmb {
        std::string subject;
        bool glasses;
        std::string label;
        std::vector<float> embOuter;
        std::vector<float> embCenter;
    };
    std::vector<ImgEmb> all;

    for (auto& [subj, variants] : subjects) {
        for (auto& [which, path] : variants) {
            dlib::matrix<dlib::rgb_pixel> img;
            try {
                // dlib's load_image only takes a narrow string, which on MSVC is
                // local-codepage bytes (GBK). Windows opens GBK-encoded filenames
                // fine, so this works; the subject/glasses matching above uses the
                // codepage-agnostic wstring(), so grouping is unaffected by it.
                dlib::load_image(img, path.string());
            } catch (const std::exception& e) {
                std::cout << "  [skip] " << path.string() << ": " << e.what() << "\n";
                continue;
            }
            // Downscale very large images to speed up SCRFD (1024-1080 wide is
            // beyond the 640² the detector expects — it still works but slower).
            const long maxSide = 640;
            long w = img.nc(), h = img.nr();
            if (std::max(w, h) > maxSide) {
                double s = static_cast<double>(maxSide) / std::max(w, h);
                dlib::matrix<dlib::rgb_pixel> small(static_cast<long>(h * s), static_cast<long>(w * s));
                dlib::resize_image(img, small);
                img = small;
            }

            auto det = detector.DetectLargestFace(img);
            if (!det) {
                std::cout << "  [no-face] " << path.filename().string() << "\n";
                continue;
            }
            dlib::full_object_detection lmk;
            dlib::rectangle rect(static_cast<long>(det->x1), static_cast<long>(det->y1),
                                 static_cast<long>(det->x2), static_cast<long>(det->y2));
            if (!landmark.DetectLandmarks(img, rect, lmk)) {
                std::cout << "  [no-landmarks] " << path.filename().string() << "\n";
                continue;
            }

            ImgEmb e;
            e.subject = subj;
            e.glasses = (which == "glasses");
            e.label = subj + (e.glasses ? " 戴" : " 不戴");
            e.embOuter = recognizer.ComputeEmbedding(img, lmk, facelogin::AlignMode::OuterEye);
            e.embCenter = recognizer.ComputeEmbedding(img, lmk, facelogin::AlignMode::EyeCenter);
            if (e.embOuter.empty() || e.embCenter.empty()) {
                std::cout << "  [no-embedding] " << path.filename().string() << "\n";
                continue;
            }
            all.push_back(std::move(e));
        }
    }

    // --- Print the distance table ---
    std::cout << "\n=== 距离表 (欧氏距离，越低越像) ===\n\n";
    std::cout << "跨眼镜(同人 戴 vs 不戴) / 基线(同人同外观) / 异人:\n\n";
    std::cout << std::fixed << std::setprecision(4);

    auto dist = [](const std::vector<float>& a, const std::vector<float>& b) {
        return facelogin::OnnxRecognizer::Distance(a, b);
    };

    for (size_t i = 0; i < all.size(); i++) {
        for (size_t j = i + 1; j < all.size(); j++) {
            const auto& a = all[i];
            const auto& b = all[j];
            std::string rel;
            if (a.subject == b.subject) {
                rel = (a.glasses != b.glasses) ? "跨眼镜" : "基线";
            } else {
                rel = "异人";
            }
            float dOuter = dist(a.embOuter, b.embOuter);
            float dCenter = dist(a.embCenter, b.embCenter);
            std::cout << "  [" << rel << "] " << a.label << " vs " << b.label
                      << "\n      外眼角=" << dOuter << "   中心=" << dCenter
                      << (dCenter < dOuter ? "   ←中心更低" : "") << "\n";
        }
    }

    std::cout << "\n判定: 对比跨眼镜行——若 中心 < 外眼角 明显，则眼睛中心对齐有效；\n"
              << "若两者接近，说明对齐不是主因，需回查活体/检测环节。\n";
    return 0;
}
