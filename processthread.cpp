#include "processthread.h"
#include "arrayqueue.h"
#include "MvCamera.h"
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QTextStream>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <cstring>

namespace {

constexpr int DISPLAY_INTERVAL_MS = 66;     // 预览刷新间隔（约 15 fps）
constexpr double PIXEL_TO_MM = 0.177;       // 像素到毫米的换算系数
constexpr float NMS_IOU_THRESHOLD = 0.5f;   // 切片合并时的 NMS 阈值
constexpr float FILTER_MIN_CONFIDENCE = 0.3f;  // 面积过滤时的最低置信度

struct DefectClass
{
    const char* name;      // 英文名（检测框标签、CSV）
    const char* nameZh;    // 中文名（缺陷列表）
};

// 类别名称，下标为模型类别 ID；换模型时按训练时的类别顺序修改
const DefectClass DEFECT_CLASSES[] = {
    {"Shaving", "刨花"},
    {"Dust", "灰尘"},
    {"Oil", "油污"},
    {"Black", "黑点"},
    {"Damage", "破坏"},
    {"Pollution", "污染"},
};

QString className(int classId)
{
    if (classId >= 0 && classId < int(std::size(DEFECT_CLASSES)))
        return QString::fromUtf8(DEFECT_CLASSES[classId].name);
    return QString("Class_%1").arg(classId);
}

QString classNameZh(int classId)
{
    if (classId >= 0 && classId < int(std::size(DEFECT_CLASSES)))
        return QString::fromUtf8(DEFECT_CLASSES[classId].nameZh);
    return QString("未知缺陷%1").arg(classId);
}

void appendDetection(trtyolo::DetectRes& dst, const trtyolo::Box& box, int classId, float score)
{
    dst.boxes.push_back(box);
    dst.classes.push_back(classId);
    dst.scores.push_back(score);
    dst.num++;
}

/// 把紧密排列的像素数据逐行拷进新 QImage（QImage 每行按 4 字节对齐）
QImage copyPacked(const unsigned char* data, uint64_t len, int width, int height,
                  QImage::Format format, int bytesPerPixel)
{
    const size_t rowBytes = size_t(width) * bytesPerPixel;
    if (len < rowBytes * height)
    {
        qWarning() << "ProcessThread - Frame data too short:" << len << "bytes for" << width << "x" << height;
        return QImage();
    }

    QImage image(width, height, format);
    if (image.isNull())
        return image;
    for (int y = 0; y < height; ++y)
        std::memcpy(image.scanLine(y), data + rowBytes * y, rowBytes);
    return image;
}

} // namespace

ProcessThread::ProcessThread(ArrayQueue& queue, CMvCamera& camera, trtyolo::DetectModel* detector,
                             const ProcessConfig& config, QObject* parent)
    : QThread(parent)
    , m_queue(queue)
    , m_camera(camera)
    , m_detector(detector)
    , m_config(config)
{
}

ProcessThread::~ProcessThread()
{
    requestInterruption();
    wait();
}

/**
 * @brief 线程主函数：取帧 → 转换 → 检测 → 拼接保存，空闲时维护 PLC 连接
 */
void ProcessThread::run()
{
    qDebug() << "ProcessThread started.";

    if (!m_config.plcIp.isEmpty())
    {
        m_plc = std::make_unique<PLCManager>();
        if (!m_plc->connectToPLC(m_config.plcIp, m_config.plcRack, m_config.plcSlot))
        {
            qWarning() << "ProcessThread - PLC connection failed, will retry:" << m_plc->getLastError();
            m_plcLostReported = true;
        }
        m_lastPlcActivity = QDateTime::currentMSecsSinceEpoch();
    }
    else
    {
        qWarning() << "ProcessThread - PLC IP is empty, skipping PLC connection";
    }

    if (m_config.framesPerBoard <= 0)
    {
        qWarning() << "ProcessThread - Invalid frames per board:" << m_config.framesPerBoard;
    }

    ImageNode node;
    node.pData = std::make_unique<unsigned char[]>(m_queue.bufferSize());

    QList<QImage> boardFrames;
    int boardHeight = 0;          // 已拼接帧的总高度，即下一帧在拼接图中的 y 偏移
    qint64 detectionMs = 0;
    qint64 busyMs = 0;            // 本板处理耗时（不含等待取帧）
    QElapsedTimer displayTimer;
    QElapsedTimer frameTimer;

    while (!isInterruptionRequested())
    {
        if (m_queue.poll(node, 50) != ArrayQueue::OK)
        {
            servicePlc();
            continue;
        }
        frameTimer.start();

        QImage image = toImage(node);
        if (image.isNull())
        {
            qWarning() << "ProcessThread - Failed to convert frame" << node.nFrameNum;
            continue;
        }

        if (boardFrames.isEmpty())
        {
            emit boardStarted();
        }

        // 缺陷检测（仅彩色图像）
        if (m_config.detect && m_detector && image.format() == QImage::Format_RGB888)
        {
            QElapsedTimer detectionTimer;
            detectionTimer.start();

            trtyolo::DetectRes result;
            if (detectDefects(image, result))
            {
                if (m_config.filterByArea && result.num > 0)
                {
                    result = filterDefectsByArea(result, m_config.minArea);
                }

                if (result.num > 0)
                {
                    emit defectsFound(drawDetectionBoxes(image, result));
                    m_boardHasDefect = true;

                    if (m_config.saveCsv)
                    {
                        // 帧内坐标 → 拼接图坐标（拼接按帧顺序纵向排列，宽度一致）
                        for (int i = 0; i < result.num; ++i)
                        {
                            const trtyolo::Box& b = result.boxes[i];
                            appendDetection(m_boardDefects,
                                            trtyolo::Box(b.left, b.top + boardHeight, b.right, b.bottom + boardHeight),
                                            result.classes[i], result.scores[i]);
                        }
                    }
                }
            }
            else
            {
                qWarning() << "ProcessThread - Defect detection failed for frame" << node.nFrameNum;
            }
            detectionMs += detectionTimer.elapsed();
        }

        boardFrames.append(image);
        boardHeight += image.height();

        if (!displayTimer.isValid() || displayTimer.elapsed() >= DISPLAY_INTERVAL_MS)
        {
            displayTimer.start();
            const QSize& target = m_config.displaySize;
            const bool shrink = target.isValid() &&
                                (image.width() > target.width() || image.height() > target.height());
            emit frameReady(shrink ? image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation) : image);
        }

        busyMs += frameTimer.elapsed();

        if (m_config.framesPerBoard > 0 && boardFrames.size() >= m_config.framesPerBoard)
        {
            frameTimer.start();
            finishBoard(boardFrames, detectionMs);
            busyMs += frameTimer.elapsed();
            qDebug() << "ProcessThread - Board finished:" << boardFrames.size() << "frames, busy" << busyMs
                     << "ms, detection" << detectionMs << "ms";

            boardFrames.clear();
            boardHeight = 0;
            detectionMs = 0;
            busyMs = 0;

            // 连续来帧时 poll() 不会超时，在板与板之间维护 PLC 连接
            servicePlc();
        }
    }

    m_plc.reset();
    qDebug() << "ProcessThread exiting.";
}

/**
 * @brief 把队列中的一帧转成 QImage：Mono8 → Grayscale8，其余格式 → RGB888
 */
QImage ProcessThread::toImage(const ImageNode& node)
{
    const unsigned char* data = node.pData.get();
    uint64_t len = node.nFrameLen;
    int width = int(node.nWidth);
    int height = int(node.nHeight);
    MvGvspPixelType type = node.enPixelType;

    if (width <= 0 || height <= 0)
    {
        return QImage();
    }

    // 解码缓冲区按最大 4 字节/像素准备，帧尺寸不变时只分配一次
    const size_t maxDecoded = size_t(width) * height * 4;
    if (m_decodeBufferSize < maxDecoded)
    {
        m_decodeBuffer = std::make_unique<unsigned char[]>(maxDecoded);
        m_decodeBufferSize = maxDecoded;
    }

    // HB 无损压缩：先解码成普通像素格式
    if (m_config.hbDecode && (unsigned(type) & MV_GVSP_PIX_CUSTOM))
    {
        MV_CC_HB_DECODE_PARAM decode = {};
        decode.pSrcBuf = const_cast<unsigned char*>(data);
        decode.nSrcLen = static_cast<unsigned int>(len);
        decode.pDstBuf = m_decodeBuffer.get();
        decode.nDstBufSize = static_cast<unsigned int>(m_decodeBufferSize);
        const int ret = m_camera.HBDecode(&decode);
        if (ret != MV_OK)
        {
            qWarning() << "ProcessThread - HB decode failed, error code:" << Qt::hex << ret;
            return QImage();
        }
        data = decode.pDstBuf;
        len = decode.nDstBufLen;
        width = int(decode.nWidth);
        height = int(decode.nHeight);
        type = decode.enDstPixelType;
    }

    if (type == PixelType_Gvsp_Mono8)
    {
        return copyPacked(data, len, width, height, QImage::Format_Grayscale8, 1);
    }
    if (type == PixelType_Gvsp_RGB8_Packed)
    {
        return copyPacked(data, len, width, height, QImage::Format_RGB888, 3);
    }

    // Bayer 等其他格式：由 SDK 转成 RGB8。行对齐时直接写进 QImage，否则经过临时缓冲区
    QImage image(width, height, QImage::Format_RGB888);
    if (image.isNull())
    {
        return image;
    }
    const size_t rowBytes = size_t(width) * 3;
    const bool direct = size_t(image.bytesPerLine()) == rowBytes;
    unsigned char* dst = direct ? image.bits() : m_decodeBuffer.get();
    std::unique_ptr<unsigned char[]> scratch;
    if (!direct && data == m_decodeBuffer.get())
    {
        // HB 解码结果占用了 m_decodeBuffer，另分配一块
        scratch = std::make_unique<unsigned char[]>(rowBytes * height);
        dst = scratch.get();
    }

    MV_CC_PIXEL_CONVERT_PARAM_EX convert = {};
    convert.nWidth = width;
    convert.nHeight = height;
    convert.pSrcData = const_cast<unsigned char*>(data);
    convert.nSrcDataLen = static_cast<unsigned int>(len);
    convert.enSrcPixelType = type;
    convert.enDstPixelType = PixelType_Gvsp_RGB8_Packed;
    convert.pDstBuffer = dst;
    convert.nDstBufferSize = static_cast<unsigned int>(rowBytes * height);

    const int ret = m_camera.ConvertPixelType(&convert);
    if (ret != MV_OK)
    {
        qWarning() << "ProcessThread - ConvertPixelType failed, pixel type" << Qt::hex << unsigned(type)
                   << "error code:" << ret;
        return QImage();
    }

    if (!direct)
    {
        for (int y = 0; y < height; ++y)
            std::memcpy(image.scanLine(y), dst + rowBytes * y, rowBytes);
    }
    return image;
}

/**
 * @brief YOLO 检测，按 tileSize 切片；整图检测直接引用原图内存，切片逐行拷进复用的切片缓冲区
 */
bool ProcessThread::detectDefects(const QImage& rgbImage, trtyolo::DetectRes& result)
{
    // 模型输入为 RGB，与 QImage::Format_RGB888 一致，无需通道交换
    auto* base = const_cast<uchar*>(rgbImage.constBits());
    const size_t pitch = size_t(rgbImage.bytesPerLine());
    const int width = rgbImage.width();
    const int height = rgbImage.height();
    const int tileSize = m_config.tileSize;

    try
    {
        if (tileSize <= 0 || (width <= tileSize && height <= tileSize))
        {
            result = m_detector->predict(trtyolo::Image(base, width, height, pitch));
            return true;
        }

        const std::vector<Tile> tiles = sliceImage(width, height, tileSize, m_config.overlapRatio);
        std::vector<trtyolo::DetectRes> tileResults;
        tileResults.reserve(tiles.size());
        for (const Tile& tile : tiles)
        {
            const size_t tilePitch = size_t(tile.width) * 3;
            m_tileBuffer.resize(tilePitch * tile.height);
            const uchar* src = base + size_t(tile.y) * pitch + size_t(tile.x) * 3;
            for (int y = 0; y < tile.height; ++y)
                std::memcpy(m_tileBuffer.data() + tilePitch * y, src + pitch * y, tilePitch);
            tileResults.push_back(m_detector->predict(trtyolo::Image(m_tileBuffer.data(), tile.width, tile.height)));
        }

        result = mergeDetectionResults(tileResults, tiles, width, height);
        return true;
    }
    catch (const std::exception& e)
    {
        qCritical() << "ProcessThread - Detection exception:" << e.what();
        return false;
    }
}

/**
 * @brief 按 tileSize 和重叠比例切片；边缘切片向内平移以保持完整尺寸
 */
std::vector<ProcessThread::Tile> ProcessThread::sliceImage(int width, int height, int tileSize, double overlapRatio)
{
    std::vector<Tile> tiles;
    const int step = std::max(1, static_cast<int>(tileSize * (1.0 - overlapRatio)));

    for (int y = 0;; y += step)
    {
        const int startY = std::max(0, std::min(y, height - tileSize));
        const int endY = std::min(startY + tileSize, height);

        for (int x = 0;; x += step)
        {
            const int startX = std::max(0, std::min(x, width - tileSize));
            const int endX = std::min(startX + tileSize, width);
            tiles.push_back({startX, startY, endX - startX, endY - startY});
            if (endX >= width)
                break;
        }

        if (endY >= height)
            break;
    }
    return tiles;
}

/**
 * @brief 把切片结果映射回原图坐标并做 NMS
 */
trtyolo::DetectRes ProcessThread::mergeDetectionResults(const std::vector<trtyolo::DetectRes>& tileResults,
                                                        const std::vector<Tile>& tiles, int imageWidth, int imageHeight)
{
    trtyolo::DetectRes merged;
    for (size_t i = 0; i < tileResults.size(); ++i)
    {
        const trtyolo::DetectRes& r = tileResults[i];
        const Tile& tile = tiles[i];
        for (int j = 0; j < r.num; ++j)
        {
            const trtyolo::Box& b = r.boxes[j];
            const trtyolo::Box mapped(b.left + tile.x, b.top + tile.y, b.right + tile.x, b.bottom + tile.y);
            // 丢弃超出原图范围的框
            if (mapped.left >= 0 && mapped.top >= 0 && mapped.right <= imageWidth && mapped.bottom <= imageHeight)
                appendDetection(merged, mapped, r.classes[j], r.scores[j]);
        }
    }
    return merged.num > 0 ? applyNMS(merged, NMS_IOU_THRESHOLD) : merged;
}

float ProcessThread::calculateIoU(const trtyolo::Box& box1, const trtyolo::Box& box2)
{
    const float left = std::max(box1.left, box2.left);
    const float top = std::max(box1.top, box2.top);
    const float right = std::min(box1.right, box2.right);
    const float bottom = std::min(box1.bottom, box2.bottom);
    if (left >= right || top >= bottom)
        return 0.0f;

    const float intersection = (right - left) * (bottom - top);
    const float area1 = (box1.right - box1.left) * (box1.bottom - box1.top);
    const float area2 = (box2.right - box2.left) * (box2.bottom - box2.top);
    const float unionArea = area1 + area2 - intersection;
    return unionArea > 0.0f ? intersection / unionArea : 0.0f;
}

/**
 * @brief 同类别 NMS：按置信度降序，抑制 IoU 超过阈值的框
 */
trtyolo::DetectRes ProcessThread::applyNMS(const trtyolo::DetectRes& result, float iouThreshold)
{
    std::vector<int> order(result.num);
    for (int i = 0; i < result.num; ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&result](int a, int b) {
        return result.scores[a] > result.scores[b];
    });

    std::vector<bool> suppressed(result.num, false);
    for (size_t i = 0; i < order.size(); ++i)
    {
        const int a = order[i];
        if (suppressed[a])
            continue;
        for (size_t j = i + 1; j < order.size(); ++j)
        {
            const int b = order[j];
            if (!suppressed[b] && result.classes[a] == result.classes[b] &&
                calculateIoU(result.boxes[a], result.boxes[b]) > iouThreshold)
            {
                suppressed[b] = true;
            }
        }
    }

    trtyolo::DetectRes kept;
    for (int i = 0; i < result.num; ++i)
    {
        if (!suppressed[i])
            appendDetection(kept, result.boxes[i], result.classes[i], result.scores[i]);
    }
    return kept;
}

/**
 * @brief 保留置信度 ≥ 0.3 且面积 ≥ minArea 的检测框
 */
trtyolo::DetectRes ProcessThread::filterDefectsByArea(const trtyolo::DetectRes& result, int minArea)
{
    if (minArea <= 0)
        return result;

    trtyolo::DetectRes filtered;
    for (int i = 0; i < result.num; ++i)
    {
        const trtyolo::Box& b = result.boxes[i];
        const int area = int(b.right - b.left) * int(b.bottom - b.top);
        if (result.scores[i] >= FILTER_MIN_CONFIDENCE && area >= minArea)
            appendDetection(filtered, b, result.classes[i], result.scores[i]);
    }
    return filtered;
}

/**
 * @brief 在图像上原地绘制检测框，返回每个缺陷的文字描述
 */
QStringList ProcessThread::drawDetectionBoxes(QImage& image, const trtyolo::DetectRes& result) const
{
    static const QColor colors[] = {
        QColor(255, 0, 0), QColor(0, 255, 0), QColor(0, 0, 255), QColor(255, 255, 0), QColor(255, 0, 255),
        QColor(0, 255, 255), QColor(255, 128, 0), QColor(128, 0, 255), QColor(0, 128, 255), QColor(128, 255, 0),
    };

    QStringList descriptions;
    descriptions.reserve(result.num);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont font = painter.font();
    font.setPointSize(10);
    font.setBold(true);
    painter.setFont(font);
    const QFontMetrics fm(font);

    for (int i = 0; i < result.num; ++i)
    {
        const trtyolo::Box& box = result.boxes[i];
        const int classId = result.classes[i];
        const QColor color = colors[std::abs(classId) % std::size(colors)];

        painter.setPen(QPen(color, 3));
        painter.drawRect(QRectF(box.left, box.top, box.right - box.left, box.bottom - box.top));

        const QString label = QString("%1: %2%").arg(className(classId)).arg(result.scores[i] * 100, 0, 'f', 1);
        QRect textRect = fm.boundingRect(label);
        textRect.moveTopLeft(QPoint(int(box.left), int(box.top) - textRect.height() - 2));
        painter.fillRect(textRect, color);
        painter.setPen(Qt::white);
        painter.drawText(textRect, Qt::AlignCenter, label);

        descriptions << QString("%1 - 尺寸: %2 mm × %3 mm")
                            .arg(classNameZh(classId))
                            .arg((box.right - box.left) * PIXEL_TO_MM, 0, 'f', 2)
                            .arg((box.bottom - box.top) * PIXEL_TO_MM, 0, 'f', 2);
    }
    return descriptions;
}

/**
 * @brief 一块板的帧收齐：通知 PLC，拼接，异步保存图像和 CSV，重置板状态
 */
void ProcessThread::finishBoard(const QList<QImage>& frames, qint64 detectionMs)
{
    if (detectionMs > 0)
    {
        emit detectionTimeUpdated(detectionMs);
    }

    if (m_boardHasDefect)
    {
        sendDefectSignal();
    }

    QImage stitched = verticalStitchImages(frames);
    if (stitched.isNull())
    {
        qWarning() << "ProcessThread - Failed to stitch images.";
    }
    else
    {
        const QString imageName = QDateTime::currentDateTime().toString("yyyy-MM-dd-HH-mm-ss-zzz") + ".jpg";
        const QString basePath = m_config.savePath;
        const bool saveCsv = m_config.saveCsv && m_boardDefects.num > 0;
        trtyolo::DetectRes defects = std::move(m_boardDefects);

        // 图像和 CSV 在同一个任务里按顺序保存，图像失败则不写 CSV
        (void)QtConcurrent::run([stitched = std::move(stitched), basePath, imageName, saveCsv,
                                 defects = std::move(defects)]() {
            if (basePath.isEmpty())
            {
                qWarning() << "ProcessThread (async) - Save path is empty";
                return;
            }
            const QString fullPath = QDir(basePath).filePath(imageName);
            if (!QDir().mkpath(basePath))
            {
                qCritical() << "ProcessThread (async) - Failed to create save directory:" << basePath;
                return;
            }
            if (!stitched.save(fullPath, "JPG", 100))
            {
                qCritical() << "ProcessThread (async) - Failed to save stitched image:" << fullPath;
                return;
            }
            qDebug() << "ProcessThread (async) - Stitched image saved:" << imageName;

            if (saveCsv && !saveDefectsToCSV(imageName, defects, basePath))
            {
                qWarning() << "ProcessThread (async) - Failed to save defects to CSV";
            }
        });
    }

    m_boardHasDefect = false;
    m_boardDefects = trtyolo::DetectRes();
}

/**
 * @brief 纵向拼接，逐行拷贝；宽度不一致时居中并以黑色填充
 */
QImage ProcessThread::verticalStitchImages(const QList<QImage>& images)
{
    if (images.isEmpty())
        return QImage();

    const QImage::Format format = images.first().format();
    int width = 0;
    int height = 0;
    for (const QImage& img : images)
    {
        width = std::max(width, img.width());
        height += img.height();
    }

    QImage result(width, height, format);
    if (result.isNull())
        return result;

    const int bytesPerPixel = result.depth() / 8;
    bool needsFill = false;
    for (const QImage& img : images)
        needsFill |= img.width() != width;
    if (needsFill)
        result.fill(Qt::black);

    int y = 0;
    for (const QImage& frame : images)
    {
        const QImage src = frame.format() == format ? frame : frame.convertToFormat(format);
        const size_t offset = size_t((width - src.width()) / 2) * bytesPerPixel;
        const size_t rowBytes = size_t(src.width()) * bytesPerPixel;
        for (int row = 0; row < src.height(); ++row)
            std::memcpy(result.scanLine(y + row) + offset, src.constScanLine(row), rowBytes);
        y += src.height();
    }
    return result;
}

/**
 * @brief 把缺陷追加到每日 CSV（defects_yyyy-MM-dd.csv），新文件先写表头
 */
bool ProcessThread::saveDefectsToCSV(const QString& imageName, const trtyolo::DetectRes& defects, const QString& basePath)
{
    const QString csvPath = QDir(basePath).filePath(
        QString("defects_%1.csv").arg(QDate::currentDate().toString("yyyy-MM-dd")));
    const bool isNewFile = !QFile::exists(csvPath);

    QFile csvFile(csvPath);
    if (!csvFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        qCritical() << "saveDefectsToCSV() - Failed to open CSV file:" << csvPath;
        return false;
    }

    QTextStream out(&csvFile);
    out.setEncoding(QStringConverter::Utf8);
    if (isNewFile)
    {
        out << "Timestamp,ImageName,DefectClass,ClassName,Left,Top,Right,Bottom,Confidence\n";
    }

    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    for (int i = 0; i < defects.num; ++i)
    {
        const trtyolo::Box& box = defects.boxes[i];
        const int classId = defects.classes[i];
        out << timestamp << ',' << imageName << ',' << classId << ',' << className(classId) << ','
            << box.left << ',' << box.top << ',' << box.right << ',' << box.bottom << ','
            << QString::number(defects.scores[i], 'f', 4) << '\n';
    }
    return true;
}

/**
 * @brief 空闲时和每块板结束后调用：已连接则定时保活，断线则定时重连
 */
void ProcessThread::servicePlc()
{
    if (!m_plc)
        return;

    const bool connected = m_plc->isConnected();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastPlcActivity < (connected ? PLC_KEEPALIVE_INTERVAL_MS : PLC_RECONNECT_INTERVAL_MS))
        return;
    m_lastPlcActivity = now;

    if (connected)
    {
        if (!m_plc->ping(PLC_DEFECT_BYTE))
        {
            qWarning() << "ProcessThread - PLC keepalive failed, will reconnect:" << m_plc->getLastError();
            m_plcLostReported = true;
        }
    }
    else if (m_plc->reconnect())
    {
        qDebug() << "ProcessThread - PLC reconnected";
        m_plcLostReported = false;
    }
    else if (!m_plcLostReported)
    {
        qWarning() << "ProcessThread - PLC reconnect failed:" << m_plc->getLastError();
        m_plcLostReported = true;
    }
}

/**
 * @brief 置位 V8080.0 通知 PLC 本板有缺陷
 *
 * 断线时直接放弃（重连交给 servicePlc() 定时进行，避免每块板都等连接超时），断线期间只记一次日志；
 * 已连接但写失败时（如 PLC 刚重启）立即重连并重试一次。
 */
void ProcessThread::sendDefectSignal()
{
    if (!m_plc)
        return;

    if (!m_plc->isConnected())
    {
        if (!m_plcLostReported)
        {
            qWarning() << "ProcessThread - PLC not connected, defect signals dropped until reconnect";
            m_plcLostReported = true;
        }
        return;
    }

    bool sent = m_plc->writeVBit(PLC_DEFECT_BYTE, PLC_DEFECT_BIT, true);
    if (!sent && m_plc->reconnect())
    {
        sent = m_plc->writeVBit(PLC_DEFECT_BYTE, PLC_DEFECT_BIT, true);
    }
    m_lastPlcActivity = QDateTime::currentMSecsSinceEpoch();

    if (sent)
    {
        qDebug() << "ProcessThread - PLC defect signal sent";
        m_plcLostReported = false;
        return;
    }
    qWarning() << "ProcessThread - Failed to send PLC defect signal:" << m_plc->getLastError();
    m_plcLostReported = true;
}
