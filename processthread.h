#ifndef PROCESSTHREAD_H
#define PROCESSTHREAD_H

#define NOMINMAX  // 防止Windows.h定义min/max宏与std::min/max冲突

#include <QThread>
#include <QPainter>
#include <QDateTime>
#include <QElapsedTimer>
#include <QImage>
#include <QtConcurrent/QtConcurrent>
#include <memory>
#include "trtyolo.hpp"
#include "PLCManager.h"
#include <opencv2/opencv.hpp>

class MainWindow;

/**
 * @brief 图像处理线程类
 *
 * 负责从队列中取出图像数据，进行格式转换、拼接和保存
 * 采用消费者模式，与 ImageCallBack (生产者) 协同工作
 */
class ProcessThread : public QThread
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param mainwindow 主窗口指针
     * @param parent 父对象指针
     */
    explicit ProcessThread(MainWindow* mainwindow, QObject* parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~ProcessThread() override = default;

protected:
    /**
     * @brief 线程主函数
     *
     * 执行以下任务:
     * 1. 从队列中取出图像数据
     * 2. 根据像素格式进行转换 (Bayer8 → RGB)
     * 3. 累积图像到拼接列表
     * 4. 达到指定帧数后执行垂直拼接
     * 5. 保存拼接后的图像到文件
     */
    void run() override;

private:
    /**
     * @brief 垂直拼接多张图像
     * @param images 待拼接的图像列表
     * @return 拼接后的图像，失败返回空图像
     */
    QImage verticalStitchImages(const QList<QImage>& images) const;

    /**
     * @brief 将 Bayer8 格式转换为 RGB 格式
     * @param bayerData Bayer8 原始数据
     * @param width 图像宽度
     * @param height 图像高度
     * @param rgbImage 输出的 RGB 图像
     * @return true-成功, false-失败
     */
    bool convertBayer8ToRGB(const unsigned char* bayerData,
                           int width,
                           int height,
                           QImage& rgbImage);

    /**
     * @brief 更新 UI 显示图像
     * @param image 待显示的图像
     */
    void updateDisplayImage(const QImage& image);

    /**
     * @brief 保存拼接后的图像
     * @param stitchedImage 拼接后的图像
     * @param basePath 保存路径
     * @return true-成功, false-失败
     */
    bool saveStitchedImage(const QImage& stitchedImage, const QString& basePath);

    /**
     * @brief 生成带时间戳的文件名（包含毫秒）
     * @return 格式化的文件名 (例: 2025-10-21-14-30-25-123.jpg)
     */
    QString generateTimestampFilename() const;

    /**
     * @brief 将 QImage 转换为 OpenCV Mat 格式
     * @param qImage 输入的 QImage
     * @return OpenCV Mat 对象
     */
    cv::Mat qImageToMat(const QImage& qImage);

    /**
     * @brief 将图像切片为多个小块
     * @param cvImage OpenCV 格式的输入图像
     * @param tileSize 切片尺寸（正方形边长）
     * @param overlapRatio 切片重叠比例（0.0-1.0）
     * @return 切片列表，每个切片包含图像和其在原图中的位置信息
     */
    struct TileInfo {
        cv::Mat image;      // 切片图像
        int startX;         // 在原图中的起始X坐标
        int startY;         // 在原图中的起始Y坐标
        int endX;           // 在原图中的结束X坐标
        int endY;           // 在原图中的结束Y坐标
    };
    std::vector<TileInfo> sliceImage(const cv::Mat& cvImage, int tileSize, double overlapRatio);

    /**
     * @brief 执行 YOLO 缺陷检测（支持切片）
     * @param rgbImage RGB 格式的图像
     * @param tileSize 切片尺寸（0表示不切片，直接检测整图）
     * @param overlapRatio 切片重叠比例
     * @param result 输出的检测结果（坐标已映射回原图）
     * @return true-检测成功, false-检测失败
     */
    bool detectDefects(const QImage& rgbImage, int tileSize, double overlapRatio, trtyolo::DetectRes& result);

    /**
     * @brief 合并切片检测结果
     * @param tileResults 各个切片的检测结果
     * @param tiles 切片信息列表
     * @param imageWidth 原图宽度
     * @param imageHeight 原图高度
     * @return 合并后的检测结果（坐标已映射回原图）
     */
    trtyolo::DetectRes mergeDetectionResults(const std::vector<trtyolo::DetectRes>& tileResults,
                                             const std::vector<TileInfo>& tiles,
                                             int imageWidth,
                                             int imageHeight);

    /**
     * @brief 非极大值抑制（NMS）算法，去除重复的检测框
     * @param result 原始检测结果
     * @param iouThreshold IoU阈值，当两个框的IoU大于该值时，保留置信度更高的框
     * @return NMS处理后的检测结果
     */
    trtyolo::DetectRes applyNMS(const trtyolo::DetectRes& result, float iouThreshold = 0.5f);

    /**
     * @brief 计算两个检测框的IoU（交并比）
     * @param box1 第一个检测框
     * @param box2 第二个检测框
     * @return IoU值（0.0-1.0之间）
     */
    float calculateIoU(const trtyolo::Box& box1, const trtyolo::Box& box2);

    /**
     * @brief 在图像上绘制检测框
     * @param image 待绘制的图像
     * @param result 检测结果
     * @return 绘制后的图像
     */
    QImage drawDetectionBoxes(const QImage& image, const trtyolo::DetectRes& result);

    /**
     * @brief 根据像素面积过滤检测结果
     * @param result 原始检测结果
     * @param minArea 最小像素面积阈值（小于该值的缺陷将被剔除）
     * @return 过滤后的检测结果
     */
    trtyolo::DetectRes filterDefectsByArea(const trtyolo::DetectRes& result, int minArea);

    /**
     * @brief 初始化PLC连接
     * @param ip PLC的IP地址
     * @param rack 机架号
     * @param slot 槽号
     * @return 成功返回true，失败返回false
     */
    bool initializePLC(const QString& ip, int rack, int slot);

    /**
     * @brief 断开PLC连接
     */
    void disconnectPLC();

    /**
     * @brief 生成每日CSV文件名
     * @return 格式化的CSV文件名 (例: defects_2025-11-13.csv)
     */
    static QString generateCSVFilename();

    /**
     * @brief 保存缺陷信息到CSV文件（静态方法，支持异步调用）
     * @param imageName 图片文件名
     * @param detectResult 检测结果
     * @param basePath 保存路径
     * @return true-成功, false-失败
     */
    static bool saveDefectsToCSV(const QString& imageName, const trtyolo::DetectRes& detectResult, const QString& basePath);

signals:
    /**
     * @brief 检测耗时信号
     * @param elapsedMs 检测耗时（毫秒）
     */
    void detectionTimeUpdated(qint64 elapsedMs);

    /**
     * @brief 添加缺陷信息到列表信号
     * @param defectInfo 缺陷信息字符串
     */
    void addDefectInfo(const QString& defectInfo);

    /**
     * @brief 清空缺陷列表信号
     */
    void clearDefectList();

private:
    MainWindow* m_mainWindow;  // 主窗口指针（只读访问配置）
    bool m_defectDetectedInCurrentImage;  // 当前完整图像是否已检测到缺陷并发送PLC信号
    trtyolo::DetectRes m_currentImageDefects;  // 当前完整图像的所有缺陷检测结果（用于CSV保存）

    // PLC相关成员变量
    PLCManager* m_plcManager;  // PLC管理器（在当前线程中运行）
    qint64 m_lastPLCCommunicationTime;  // 上次PLC通信时间戳（毫秒）
    static constexpr int PLC_KEEPALIVE_INTERVAL_MS = 4000;  // PLC保活间隔（4秒）
};

#endif // PROCESSTHREAD_H
