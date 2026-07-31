#pragma once

#include <dlib/dnn.h>
#include <dlib/image_processing.h>
#include <dlib/image_transforms.h>

#include <string>
#include <vector>
#include <cstdint>

namespace facelogin {

// dlib face recognition network type (dlib_face_recognition_resnet_model_v1.dat)
// This produces 128-dimensional face embeddings.
//
// NOTE: This MUST exactly match the network architecture that dlib's
// pre-trained model was serialized with. Copied from dlib's
// examples/dnn_face_recognition_ex.cpp

template <template <int,template<typename>class,int,typename> class block, int N, template<typename>class BN, typename SUBNET>
using residual = dlib::add_prev1<block<N,BN,1,dlib::tag1<SUBNET>>>;

template <template <int,template<typename>class,int,typename> class block, int N, template<typename>class BN, typename SUBNET>
using residual_down = dlib::add_prev2<dlib::avg_pool<2,2,2,2,dlib::skip1<dlib::tag2<block<N,BN,2,dlib::tag1<SUBNET>>>>>>;

template <int N, template <typename> class BN, int stride, typename SUBNET>
using block  = BN<dlib::con<N,3,3,1,1,dlib::relu<BN<dlib::con<N,3,3,stride,stride,SUBNET>>>>>;

template <int N, typename SUBNET> using ares      = dlib::relu<residual<block,N,dlib::affine,SUBNET>>;
template <int N, typename SUBNET> using ares_down = dlib::relu<residual_down<block,N,dlib::affine,SUBNET>>;

template <typename SUBNET> using alevel0 = ares_down<256,SUBNET>;
template <typename SUBNET> using alevel1 = ares<256,ares<256,ares_down<256,SUBNET>>>;
template <typename SUBNET> using alevel2 = ares<128,ares<128,ares_down<128,SUBNET>>>;
template <typename SUBNET> using alevel3 = ares<64,ares<64,ares<64,ares_down<64,SUBNET>>>>;
template <typename SUBNET> using alevel4 = ares<32,ares<32,ares<32,SUBNET>>>;

using FaceNetType = dlib::loss_metric<dlib::fc_no_bias<128,dlib::avg_pool_everything<
                            alevel0<
                            alevel1<
                            alevel2<
                            alevel3<
                            alevel4<
                            dlib::max_pool<3,3,2,2,dlib::relu<dlib::affine<dlib::con<32,7,7,2,2,
                            dlib::input_rgb_image_sized<150>
                            >>>>>>>>>>>>;

class FaceRecognizer {
public:
    FaceRecognizer() = default;
    ~FaceRecognizer() = default;

    // Load the pre-trained recognition model.
    // modelPath: path to dlib_face_recognition_resnet_model_v1.dat
    bool Initialize(const std::wstring& modelPath);

    bool IsInitialized() const { return m_initialized; }

    // Compute a 128-D face embedding from an aligned face chip.
    // The input image should be a face chip (cropped and aligned to the face).
    dlib::matrix<float, 0, 1> ComputeEmbedding(
        const dlib::matrix<dlib::rgb_pixel>& faceChip);

    // Compute embedding from a full image + face with landmarks.
    // Handles alignment automatically.
    dlib::matrix<float, 0, 1> ComputeEmbedding(
        const dlib::matrix<dlib::rgb_pixel>& image,
        const dlib::full_object_detection& landmarks);

    // Compare two embeddings. Returns Euclidean distance.
    // Lower = more similar. Threshold typically 0.30-0.45.
    static float Distance(const dlib::matrix<float, 0, 1>& a,
                          const dlib::matrix<float, 0, 1>& b);

    // Compare with stored embedding bytes (for DB lookups).
    static float Distance(const dlib::matrix<float, 0, 1>& probe,
                          const float* storedEmbedding);

    // Convert embedding to/from raw float array for storage
    static std::vector<uint8_t> SerializeEmbedding(const dlib::matrix<float, 0, 1>& emb);
    static dlib::matrix<float, 0, 1> DeserializeEmbedding(const uint8_t* data);

private:
    FaceNetType m_net;
    bool m_initialized = false;
};

} // namespace facelogin
