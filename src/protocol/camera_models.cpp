#include "camera.hpp"

namespace tir {

// From InitUsbDevicePathRevisionMap (00401f10) and CreateCameraForRevision (005921c0).
const std::vector<CameraModel>& knownCameraModels()
{
    static const std::vector<CameraModel> models = {
        {0x0156, "TrackIR 4", "CameraRev5", ProtocolKind::Classic, false, 0, 0, 5},
        {0x0157, "TrackIR 5 (Rev9)", "CameraRev9", ProtocolKind::Classic, true, 640, 480, 9},
        {0x0158, "TrackIR 5 (Rev18)", "CameraRev18", ProtocolKind::Classic, true, 640, 480, 0x12},
        {0x0159, "TrackIR 5", "CameraRev35", ProtocolKind::Secure, true, 640, 480, -1},
    };
    return models;
}

const CameraModel* findCameraModel(uint16_t productId)
{
    for (const auto& m : knownCameraModels())
        if (m.productId == productId)
            return &m;
    return nullptr;
}

LensModel lensFor(const CameraModel& model, uint32_t serial)
{
    LensModel lens;
    lens.cx = model.width / 2;
    lens.cy = model.height / 2;
    if (model.protocol == ProtocolKind::Secure) {
        // Rev35_GetLensModel (005bcc00): two lens batches split by serial number.
        if (serial != 0 && serial < 400891) {
            lens.focalPx = 583.0;
            lens.k1 = -0.08626669645309448;
            lens.k2 = 0.230944;
        } else {
            lens.focalPx = 594.0;
            lens.k1 = -0.09601020067930222;
            lens.k2 = 0.4249440133571625;
        }
    } else {
        // Rev9_GetLensModel (005a2650).
        lens.focalPx = 630.0;
        lens.k1 = 0.06148223951458931;
        lens.k2 = -0.6844258308410645;
        lens.k3 = 0.014313340187072754;
    }
    return lens;
}

}  // namespace tir
