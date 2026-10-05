#ifndef PROCESSTHREAD_H
#define PROCESSTHREAD_H

#include <QThread>
#include <QImage>
#include <QSize>
#include <QStringList>
#include <memory>
#include <vector>
#include "trtyolo.hpp"
#include "PLCManager.h"

class ArrayQueue;
class CMvCamera;
struct ImageNode;

/**
 * @brief 开始采集时从界面读取的一份参数快照
 *
 * 处理线程只读这份快照，运行期间不访问任何界面控件。
 */
struct ProcessConfig
{
    int framesPerPart = 0;         // 每件拼接的帧数
    QString savePath;              // 拼接图与 CSV 的保存目录
    bool hbDecode = false;         // 相机开启了 HB 无损压缩，需要先解码
    bool detect = false;           // 是否做缺陷检测
    int tileSize = 0;              // 切片边长，0 表示整图检测
    double overlapRatio = 0.2;     // 切片重叠比例
    bool filterByArea = false;     // 是否按面积过滤
    int minArea = 0;               // 面积阈值（像素）
    bool saveCsv = false;          // 是否把缺陷写入 CSV
    QString plcIp;                 // 为空则不连接 PLC
    int plcRack = 0;
    int plcSlot = 1;
    QSize displaySize;             // 预览区域大小（物理像素），线程按此预缩放
};

/**
 * @brief 图像处理线程（消费者）
 *
 * 从队列取帧 → 转 RGB → 缺陷检测 → 累积拼接 → 异步保存，并在每件结束时通知 PLC。
 * 与界面只通过信号交互。
 */
class ProcessThread : public QThread
{
    Q_OBJECT

public:
    /**
     * @param queue    帧队列，生命周期必须长于线程
     * @param camera   用于像素格式转换和 HB 解码，生命周期必须长于线程
     * @param detector 检测模型，可为空；线程运行期间不得替换
     */
    ProcessThread(ArrayQueue& queue, CMvCamera& camera, trtyolo::DetectModel* detector,
                  const ProcessConfig& config, QObject* parent = nullptr);

    /// 请求停止并等待线程退出
    ~ProcessThread() override;

signals:
    /// 预览帧（已按 displaySize 缩放，带检测框），最多约 15 fps
    void frameReady(const QImage& image);

    /// 新一件的第一帧到达
    void partStarted();

    /// 一帧检测到的缺陷描述
    void defectsFound(const QStringList& defects);

    /// 一件所有帧的检测总耗时（毫秒）
    void detectionTimeUpdated(qint64 elapsedMs);

protected:
    void run() override;

private:
    struct Tile
    {
        int x;
        int y;
        int width;
        int height;
    };

    QImage toImage(const ImageNode& node);
    bool detectDefects(const QImage& rgbImage, trtyolo::DetectRes& result);
    QStringList drawDetectionBoxes(QImage& image, const trtyolo::DetectRes& result) const;
    void finishPart(const QList<QImage>& frames, qint64 detectionMs);

    void servicePlc();
    void sendDefectSignal();

    static std::vector<Tile> sliceImage(int width, int height, int tileSize, double overlapRatio);
    static trtyolo::DetectRes mergeDetectionResults(const std::vector<trtyolo::DetectRes>& tileResults,
                                                    const std::vector<Tile>& tiles, int imageWidth, int imageHeight);
    static trtyolo::DetectRes applyNMS(const trtyolo::DetectRes& result, float iouThreshold);
    static float calculateIoU(const trtyolo::Box& box1, const trtyolo::Box& box2);
    static trtyolo::DetectRes filterDefectsByArea(const trtyolo::DetectRes& result, int minArea);
    static QImage verticalStitchImages(const QList<QImage>& images);
    static bool saveDefectsToCSV(const QString& imageName, const trtyolo::DetectRes& defects, const QString& basePath);

    ArrayQueue& m_queue;
    CMvCamera& m_camera;
    trtyolo::DetectModel* m_detector;
    const ProcessConfig m_config;

    // 当前件的状态
    bool m_partHasDefect = false;           // 本件是否检测到缺陷（本件结束时通知 PLC）
    trtyolo::DetectRes m_partDefects;       // 本件缺陷，坐标为拼接图坐标（用于 CSV）

    // 格式转换缓冲区（仅行未对齐或 HB 解码时使用）
    std::unique_ptr<unsigned char[]> m_decodeBuffer;
    size_t m_decodeBufferSize = 0;

    // 切片缓冲区：TensorRT-YOLO 按 height × pitch 整块拷贝输入，切片必须紧密排列，不能直接引用原图
    std::vector<unsigned char> m_tileBuffer;

    // PLC（在本线程中创建和使用）
    std::unique_ptr<PLCManager> m_plc;
    qint64 m_lastPlcActivity = 0;           // 上次通信或重连尝试的时间（毫秒）
    bool m_plcLostReported = false;         // 断线日志只打印一次
    static constexpr int PLC_KEEPALIVE_INTERVAL_MS = 4000;   // 已连接时的保活间隔
    static constexpr int PLC_RECONNECT_INTERVAL_MS = 10000;  // 断线时的重连间隔（连接超时会阻塞本线程）
    static constexpr int PLC_DEFECT_BYTE = 8080;
    static constexpr int PLC_DEFECT_BIT = 0;
};

#endif // PROCESSTHREAD_H
