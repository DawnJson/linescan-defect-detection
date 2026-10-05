#include "processthread.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "arrayqueue.h"
#include <QDir>
#include <QDebug>
#include <QRegularExpression>
#include <QFile>
#include <QTextStream>
#include <Windows.h>

/**
 * @brief 构造函数
 * @param mainwindow 主窗口指针
 * @param parent 父对象指针
 */
ProcessThread::ProcessThread(MainWindow* mainwindow, QObject* parent)
    : QThread(parent)
    , m_mainWindow(mainwindow)
    , m_defectDetectedInCurrentImage(false)
    , m_plcManager(nullptr)
    , m_lastPLCCommunicationTime(0)
{
    if (!m_mainWindow)
    {
        qCritical() << "ProcessThread: mainwindow pointer is null!";
    }
}

/**
 * @brief 线程主函数
 *
 * 循环处理图像数据：从队列取出 → 格式转换 → 拼接 → 保存
 */
void ProcessThread::run()
{
    if (!m_mainWindow)
    {
        qCritical() << "ProcessThread::run() - MainWindow pointer is null, thread exiting.";
        return;
    }

    qDebug() << "ProcessThread started.";

    // 自动连接PLC（从MainWindow UI读取参数）
    if (m_mainWindow && m_mainWindow->ui)
    {
        QString ip = m_mainWindow->ui->IPEdit->text();
        int rack = m_mainWindow->ui->RackEdit->text().toInt();
        int slot = m_mainWindow->ui->SlotEdit->text().toInt();

        if (!ip.isEmpty())
        {
            if (initializePLC(ip, rack, slot))
            {
                // 初始化PLC通信时间戳
                m_lastPLCCommunicationTime = QDateTime::currentMSecsSinceEpoch();
            }
        }
        else
        {
            qWarning() << "ProcessThread: PLC IP is empty, skipping PLC connection";
        }
    }

    // 准备输出缓冲区
    std::unique_ptr<unsigned char[]> pOutData(new(std::nothrow) unsigned char[m_mainWindow->m_nImageSize]);
    if (!pOutData)
    {
        qCritical() << "ProcessThread::run() - Failed to allocate output buffer, size:" << m_mainWindow->m_nImageSize;
        return;
    }

    // 准备图像格式转换参数（仅分配一次，避免重复 new/delete）
    std::unique_ptr<MV_CC_PIXEL_CONVERT_PARAM_EX> stConvertParam(new(std::nothrow) MV_CC_PIXEL_CONVERT_PARAM_EX());
    if (!stConvertParam)
    {
        qCritical() << "ProcessThread::run() - Failed to allocate convert param struct.";
        return;
    }
    memset(stConvertParam.get(), 0, sizeof(MV_CC_PIXEL_CONVERT_PARAM_EX));

    // 图像拼接列表
    QList<QImage> imagesToStitch;

    // 检测计时器
    QElapsedTimer detectionTimer;
    qint64 totalDetectionTime = 0;  // 累计检测耗时（毫秒）

    // 获取配置参数
    const int targetImageCount = m_mainWindow->ui->AcquisitionBurstFrameCountEdit->text().toInt();
    const QString basePath = m_mainWindow->ui->imgSavePathEdit->text();

    if (targetImageCount <= 0)
    {
        qWarning() << "ProcessThread::run() - Invalid target image count:" << targetImageCount;
    }

    // 主循环：处理图像数据
    while (m_mainWindow->m_ThreadState)
    {
        // 从队列中取出图像数据
        int nFrameNum = 0;
        int nHeight = 0;
        int nWidth = 0;
        uint64_t nFrameLen = 0;

        int nRet = m_mainWindow->m_queue->poll(nFrameNum, nHeight, nWidth, pOutData.get(), nFrameLen);
        if (ArrayQueue::OK != nRet)
        {
            // 队列为空，等待数据
            Sleep(2);

            // PLC保活机制：检查是否需要发送保活信号
            if (m_plcManager && m_plcManager->isConnected())
            {
                qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
                qint64 elapsedTime = currentTime - m_lastPLCCommunicationTime;

                if (elapsedTime >= PLC_KEEPALIVE_INTERVAL_MS)
                {
                    // 执行PLC保活操作：读取指定地址的值
                    if (m_plcManager->readVBit(8080, 0))
                    {
                        // 更新最后通信时间
                        m_lastPLCCommunicationTime = currentTime;
                        qDebug() << "ProcessThread::run() - PLC keepalive successful";
                    }
                    else
                    {
                        qWarning() << "ProcessThread::run() - PLC keepalive failed:"
                                  << m_plcManager->getLastError();
                        // 即使失败也更新时间，避免频繁重试
                        m_lastPLCCommunicationTime = currentTime;
                    }
                }
            }

            continue;
        }

        // 根据像素格式处理图像
        // 注意: m_PixelFormat 存储的是下拉框索引，需要通过 UI 获取实际的像素格式字符串
        QImage currentImage;
        QString pixelFormatStr = m_mainWindow->ui->PixelFormatBox->currentText();

        if (pixelFormatStr == "BayerRG8")
        {
            // Bayer8 格式需要转换为 RGB
            if (!convertBayer8ToRGB(pOutData.get(), nWidth, nHeight, currentImage))
            {
                qWarning() << "ProcessThread::run() - Failed to convert Bayer8 to RGB for frame" << nFrameNum;
                continue;
            }
        }
        else if (pixelFormatStr == "Mono8")
        {
            // Mono8 灰度图像
            currentImage = QImage(pOutData.get(), nWidth, nHeight, QImage::Format_Grayscale8).copy();
        }
        else if (pixelFormatStr == "RGB8Packed")
        {
            // RGB8 彩色图像
            currentImage = QImage(pOutData.get(), nWidth, nHeight, QImage::Format_RGB888).copy();
        }
        else
        {
            // 默认按灰度处理
            qWarning() << "ProcessThread::run() - Unknown pixel format:" << pixelFormatStr << ", treating as Mono8";
            currentImage = QImage(pOutData.get(), nWidth, nHeight, QImage::Format_Grayscale8).copy();
        }

        // 检查图像是否有效
        if (currentImage.isNull())
        {
            qWarning() << "ProcessThread::run() - Failed to create QImage for frame" << nFrameNum;
            continue;
        }

        // 缺陷检测流程（仅针对 Bayer8 和 RGB8 格式）
        QImage displayImage = currentImage;  // 用于显示的图像
        if ((pixelFormatStr == "BayerRG8" || pixelFormatStr == "RGB8Packed") && m_mainWindow->m_enableDefectDetection && m_mainWindow->m_yoloDetector)
        {
            // 从 UI 获取切片参数
            int tileSize = 0;
            double overlapRatio = 0.2;

            // 解析切片尺寸（例如 "1024×1024" -> 1024）
            QString tileSizeStr = m_mainWindow->ui->TileSizeComboBox->currentText();
            QStringList sizeTokens = tileSizeStr.split(QRegularExpression("[×xX]"));
            if (!sizeTokens.isEmpty())
            {
                tileSize = sizeTokens[0].toInt();
            }

            // 解析重叠比例（例如 "20%" -> 0.2）
            QString overlapStr = m_mainWindow->ui->OverlapRatioCombobox->currentText();
            overlapStr.remove('%');
            overlapRatio = overlapStr.toDouble() / 100.0;

            // 开始计时检测
            detectionTimer.start();

            // 执行缺陷检测（支持切片）
            trtyolo::DetectRes detectResult;
            if (detectDefects(currentImage, tileSize, overlapRatio, detectResult))
            {
                // 【缺陷过滤】根据UI设置进行面积过滤
                if (m_mainWindow->ui->DefectFilterBox->isChecked() && detectResult.num > 0)
                {
                    // 读取面积阈值
                    int minArea = m_mainWindow->ui->FilterSizeCombobox->currentText().toInt();
                    // 执行过滤
                    detectResult = filterDefectsByArea(detectResult, minArea);
                }

                // 在图像上绘制检测框
                if (detectResult.num > 0)
                {
                    displayImage = drawDetectionBoxes(currentImage, detectResult);
                    qDebug() << "ProcessThread::run() - Detected" << detectResult.num << "defects in frame" << nFrameNum;

                    // 标记当前完整图像检测到缺陷（不立即发送PLC信号）
                    m_defectDetectedInCurrentImage = true;

                    if (m_mainWindow->ui->SaveDefectCheckBox->isChecked())
                    {
                        // 【累积缺陷结果】将当前帧的缺陷结果累积到完整图像的缺陷列表中
                        for (int i = 0; i < detectResult.num; ++i)
                        {
                            m_currentImageDefects.boxes.push_back(detectResult.boxes[i]);
                            m_currentImageDefects.classes.push_back(detectResult.classes[i]);
                            m_currentImageDefects.scores.push_back(detectResult.scores[i]);
                            m_currentImageDefects.num++;
                        }
                    }

                    // 【旧逻辑已注释】原来是检测到缺陷立即发送PLC信号
                    // // 如果当前完整图像尚未发送PLC信号，则直接发送
                    // if (!m_defectDetectedInCurrentImage && m_plcManager && m_plcManager->isConnected())
                    // {
                    //     // 向8080字节的第0位写入1
                    //     if (m_plcManager->writeVBit(8080, 0, true))
                    //     {
                    //         qDebug() << "ProcessThread::run() - PLC signal sent successfully for defect detection";
                    //         m_defectDetectedInCurrentImage = true;
                    //         // 更新PLC通信时间戳
                    //         m_lastPLCCommunicationTime = QDateTime::currentMSecsSinceEpoch();
                    //     }
                    //     else
                    //     {
                    //         qWarning() << "ProcessThread::run() - Failed to send PLC signal:"
                    //                   << m_plcManager->getLastError();
                    //     }
                    // }
                }

                // 累加检测耗时
                totalDetectionTime += detectionTimer.elapsed();
            }
            else
            {
                qWarning() << "ProcessThread::run() - Defect detection failed for frame" << nFrameNum;
                // 即使检测失败，也累加耗时
            }
        }

        // 添加到拼接列表（使用原始图像，带检测框）  currentImage不带检测框
        imagesToStitch.append(displayImage);

        // 更新 UI 显示图像（带检测框）
        updateDisplayImage(displayImage);

        // 检查是否达到拼接帧数
        if (imagesToStitch.size() >= targetImageCount && targetImageCount > 0)
        {
            // 整张图片所有帧检测完毕后，在拼接之前发送检测耗时信号
            if (totalDetectionTime > 0)
            {
                emit detectionTimeUpdated(totalDetectionTime);
                // qDebug() << "ProcessThread::run() - Total detection time:" << totalDetectionTime << "ms";
            }

            // 【整张图片所有帧检测完毕后，如果有缺陷则发送PLC信号（在拼接之前）
            if (m_defectDetectedInCurrentImage && m_plcManager && m_plcManager->isConnected())
            {
                // 向8080字节的第0位写入1
                if (m_plcManager->writeVBit(8080, 0, true))
                {
                    qDebug() << "ProcessThread::run() - PLC signal sent successfully after all frames detected (defect found)";
                    // 更新PLC通信时间戳
                    m_lastPLCCommunicationTime = QDateTime::currentMSecsSinceEpoch();
                }
                else
                {
                    qWarning() << "ProcessThread::run() - Failed to send PLC signal after all frames:"
                              << m_plcManager->getLastError();
                }
            }

            // 执行垂直拼接
            QImage stitchedImage = verticalStitchImages(imagesToStitch);

            if (!stitchedImage.isNull())
            {
                // 生成图像文件名（用于CSV记录）
                const QString imageName = generateTimestampFilename();

                // 【异步保存：图像 + CSV】合并为一个任务，保证执行顺序和数据一致性
                // 深拷贝所有需要的数据以确保线程安全
                QImage stitchedImageCopy = stitchedImage.copy();
                QString basePathCopy = basePath;
                QString imageNameCopy = imageName;

                // 拷贝缺陷检测相关数据
                bool shouldSaveCSV = m_mainWindow->ui->SaveDefectCheckBox->isChecked() && m_defectDetectedInCurrentImage && m_currentImageDefects.num > 0;
                trtyolo::DetectRes defectsCopy = m_currentImageDefects;

                QtConcurrent::run([stitchedImageCopy, basePathCopy, imageNameCopy, shouldSaveCSV, defectsCopy]() {
                    // 第一步：保存图像
                    const QString fullPath = QDir(basePathCopy).filePath(imageNameCopy);
                    bool imageSaved = false;

                    if (!stitchedImageCopy.isNull() && !basePathCopy.isEmpty())
                    {
                        if (stitchedImageCopy.save(fullPath, "JPG", 100))
                        {
                            qDebug() << "ProcessThread (async) - Stitched image saved successfully:" << imageNameCopy;
                            imageSaved = true;
                        }
                        else
                        {
                            qCritical() << "ProcessThread (async) - Failed to save stitched image:" << fullPath;
                        }
                    }
                    else
                    {
                        qWarning() << "ProcessThread (async) - Invalid parameters for image save";
                    }

                    // 第二步：只有图像保存成功，才保存 CSV
                    if (imageSaved && shouldSaveCSV)
                    {
                        if (ProcessThread::saveDefectsToCSV(imageNameCopy, defectsCopy, basePathCopy))
                        {
                            qDebug() << "ProcessThread (async) - Defects saved to CSV successfully:" << imageNameCopy
                                    << "with" << defectsCopy.num << "defect(s)";
                        }
                        else
                        {
                            qWarning() << "ProcessThread (async) - Failed to save defects to CSV";
                        }
                    }
                });

                // 清空拼接列表，准备下一批
                imagesToStitch.clear();

                // 使用信号清空缺陷列表（线程安全）
                emit clearDefectList();

                // 重置检测耗时
                totalDetectionTime = 0;

                // // 更新PLC通信时间戳
                // m_lastPLCCommunicationTime = QDateTime::currentMSecsSinceEpoch();

                // 重置缺陷检测标志和缺陷结果，为下一张完整图像做准备
                m_defectDetectedInCurrentImage = false;
                m_currentImageDefects = trtyolo::DetectRes();  // 重置为空结果
            }
            else
            {
                qWarning() << "ProcessThread::run() - Failed to stitch images.";
                imagesToStitch.clear();

                // 重置检测耗时
                totalDetectionTime = 0;

                // 重置缺陷检测标志和缺陷结果，为下一张完整图像做准备
                m_defectDetectedInCurrentImage = false;
                m_currentImageDefects = trtyolo::DetectRes();  // 重置为空结果
            }
        }
    }

    // 线程退出前断开PLC连接
    disconnectPLC();

    qDebug() << "ProcessThread exiting.";
}

/**
 * @brief 将 Bayer8 格式转换为 RGB 格式
 */
bool ProcessThread::convertBayer8ToRGB(const unsigned char* bayerData,
                                      int width,
                                      int height,
                                      QImage& rgbImage)
{
    if (!bayerData || width <= 0 || height <= 0)
    {
        qWarning() << "convertBayer8ToRGB() - Invalid parameters";
        return false;
    }

    if (!m_mainWindow || !m_mainWindow->m_MyCamera)
    {
        qCritical() << "convertBayer8ToRGB() - Camera object not available";
        return false;
    }

    // 准备转换参数
    MV_CC_PIXEL_CONVERT_PARAM_EX stConvertParam;
    memset(&stConvertParam, 0, sizeof(MV_CC_PIXEL_CONVERT_PARAM_EX));

    stConvertParam.nWidth = width;
    stConvertParam.nHeight = height;
    stConvertParam.pSrcData = const_cast<unsigned char*>(bayerData);
    stConvertParam.nSrcDataLen = width * height;
    stConvertParam.enSrcPixelType = PixelType_Gvsp_BayerRG8;
    stConvertParam.enDstPixelType = PixelType_Gvsp_RGB8_Packed;

    // 分配 RGB 缓冲区
    const unsigned int rgbBufferSize = width * height * 3;
    std::unique_ptr<unsigned char[]> pRGBBuffer(new(std::nothrow) unsigned char[rgbBufferSize]);
    if (!pRGBBuffer)
    {
        qCritical() << "convertBayer8ToRGB() - Failed to allocate RGB buffer";
        return false;
    }

    stConvertParam.pDstBuffer = pRGBBuffer.get();
    stConvertParam.nDstBufferSize = rgbBufferSize;

    // 执行转换
    int ret = m_mainWindow->m_MyCamera->ConvertPixelType(&stConvertParam);
    if (ret != MV_OK)
    {
        qWarning() << "convertBayer8ToRGB() - ConvertPixelType failed, error code:" << ret;
        return false;
    }

    // 创建 QImage
    rgbImage = QImage(pRGBBuffer.get(), width, height, QImage::Format_RGB888).copy();

    return !rgbImage.isNull();
}

/**
 * @brief 垂直拼接多张图像
 */
QImage ProcessThread::verticalStitchImages(const QList<QImage>& images) const
{
    if (images.isEmpty())
    {
        qWarning() << "verticalStitchImages() - Empty image list";
        return QImage();
    }

    // 计算拼接后图像的总尺寸
    int totalWidth = 0;
    int totalHeight = 0;

    for (const QImage& img : images)
    {
        if (img.isNull())
        {
            qWarning() << "verticalStitchImages() - Found null image in list";
            continue;
        }
        totalWidth = qMax(totalWidth, img.width());
        totalHeight += img.height();
    }

    if (totalWidth == 0 || totalHeight == 0)
    {
        qWarning() << "verticalStitchImages() - Invalid total size:" << totalWidth << "x" << totalHeight;
        return QImage();
    }

    // 创建拼接结果图像
    QImage result(totalWidth, totalHeight, QImage::Format_ARGB32);
    result.fill(Qt::transparent);

    // 垂直拼接
    QPainter painter(&result);
    int yOffset = 0;

    for (const QImage& img : images)
    {
        if (img.isNull())
        {
            continue;
        }

        // 居中绘制
        const int xOffset = (totalWidth - img.width()) / 2;
        painter.drawImage(xOffset, yOffset, img);
        yOffset += img.height();
    }

    painter.end();
    return result;
}

/**
 * @brief 更新 UI 显示图像
 */
void ProcessThread::updateDisplayImage(const QImage& image)
{
    if (!m_mainWindow || !m_mainWindow->ui || !m_mainWindow->ui->PicLabel)
    {
        return;
    }

    if (image.isNull())
    {
        return;
    }

    // 缩放图像以适应显示区域
    QPixmap pixmap = QPixmap::fromImage(image);
    QPixmap scaledPixmap = pixmap.scaled(
        m_mainWindow->ui->PicLabel->width(),
        m_mainWindow->ui->PicLabel->height(),
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation
    );

    // 更新 UI (Qt 会自动处理跨线程信号)
    m_mainWindow->ui->PicLabel->setPixmap(scaledPixmap);
}

/**
 * @brief 保存拼接后的图像
 */
bool ProcessThread::saveStitchedImage(const QImage& stitchedImage, const QString& basePath)
{
    if (stitchedImage.isNull())
    {
        qWarning() << "saveStitchedImage() - Stitched image is null";
        return false;
    }

    if (basePath.isEmpty())
    {
        qWarning() << "saveStitchedImage() - Base path is empty";
        return false;
    }

    // 生成文件名
    const QString filename = generateTimestampFilename();
    const QString fullPath = QDir(basePath).filePath(filename);

    // 保存图像
    if (!stitchedImage.save(fullPath, "JPG", 100))
    {
        qCritical() << "saveStitchedImage() - Failed to save image to" << fullPath;
        return false;
    }

    return true;
}

/**
 * @brief 生成带时间戳的文件名（包含毫秒）
 */
QString ProcessThread::generateTimestampFilename() const
{
    const QDateTime currentDateTime = QDateTime::currentDateTime();
    const QString dateTimeString = currentDateTime.toString("yyyy-MM-dd-HH-mm-ss-zzz");
    return QString("%1.jpg").arg(dateTimeString);
}

/**
 * @brief 将 QImage 转换为 OpenCV Mat 格式
 */
cv::Mat ProcessThread::qImageToMat(const QImage& qImage)
{
    if (qImage.isNull())
    {
        qWarning() << "qImageToMat() - Input QImage is null";
        return cv::Mat();
    }

    cv::Mat mat;

    switch (qImage.format())
    {
    case QImage::Format_RGB888:
    {
        // RGB888 格式，先深拷贝避免修改原图
        cv::Mat tempMat(qImage.height(), qImage.width(), CV_8UC3,
                       const_cast<uchar*>(qImage.bits()),
                       static_cast<size_t>(qImage.bytesPerLine()));
        tempMat = tempMat.clone();
        // OpenCV 使用 BGR，QImage 使用 RGB，需要转换
        cv::cvtColor(tempMat, mat, cv::COLOR_RGB2BGR);
        break;
    }
    case QImage::Format_Grayscale8:
    {
        // 灰度图
        mat = cv::Mat(qImage.height(), qImage.width(), CV_8UC1,
                     const_cast<uchar*>(qImage.bits()),
                     static_cast<size_t>(qImage.bytesPerLine()));
        mat = mat.clone();
        break;
    }
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    {
        // ARGB32 格式，先深拷贝避免修改原图
        cv::Mat tempMat(qImage.height(), qImage.width(), CV_8UC4,
                       const_cast<uchar*>(qImage.bits()),
                       static_cast<size_t>(qImage.bytesPerLine()));
        tempMat = tempMat.clone();
        // 转换为 BGR
        cv::cvtColor(tempMat, mat, cv::COLOR_BGRA2BGR);
        break;
    }
    default:
    {
        // 其他格式，先转为 RGB888 再处理
        QImage convertedImage = qImage.convertToFormat(QImage::Format_RGB888);
        cv::Mat tempMat(convertedImage.height(), convertedImage.width(), CV_8UC3,
                       const_cast<uchar*>(convertedImage.bits()),
                       static_cast<size_t>(convertedImage.bytesPerLine()));
        tempMat = tempMat.clone();
        // 转换为 BGR
        cv::cvtColor(tempMat, mat, cv::COLOR_RGB2BGR);
        break;
    }
    }

    return mat;
}

/**
 * @brief 将图像切片为多个小块
 */
std::vector<ProcessThread::TileInfo> ProcessThread::sliceImage(const cv::Mat& cvImage, int tileSize, double overlapRatio)
{
    std::vector<TileInfo> tiles;

    if (cvImage.empty())
    {
        qWarning() << "sliceImage() - Input image is empty";
        return tiles;
    }

    const int imageHeight = cvImage.rows;
    const int imageWidth = cvImage.cols;

    // 计算步长（考虑重复区域）
    const int stepSize = static_cast<int>(tileSize * (1.0 - overlapRatio));

    // qDebug() << "sliceImage() - Original image size:" << imageWidth << "x" << imageHeight;
    // qDebug() << "sliceImage() - Tile size:" << tileSize << "x" << tileSize;
    // qDebug() << "sliceImage() - Overlap ratio:" << (overlapRatio * 100) << "%";
    // qDebug() << "sliceImage() - Step size:" << stepSize;

    // 遍历图像，生成切片
    for (int y = 0; y < imageHeight; y += stepSize)
    {
        for (int x = 0; x < imageWidth; x += stepSize)
        {
            TileInfo tile;

            // 计算当前切片的起始和结束位置
            tile.startX = x;
            tile.startY = y;
            tile.endX = std::min(x + tileSize, imageWidth);
            tile.endY = std::min(y + tileSize, imageHeight);

            // 如果切片尺寸不足，向前顺延以确保切片大小
            if (tile.endX - tile.startX < tileSize && tile.endX == imageWidth)
            {
                tile.startX = std::max(0, imageWidth - tileSize);
            }
            if (tile.endY - tile.startY < tileSize && tile.endY == imageHeight)
            {
                tile.startY = std::max(0, imageHeight - tileSize);
            }

            // 更新结束位置
            tile.endX = std::min(tile.startX + tileSize, imageWidth);
            tile.endY = std::min(tile.startY + tileSize, imageHeight);

            // 创建ROI区域并复制
            cv::Rect roi(tile.startX, tile.startY, tile.endX - tile.startX, tile.endY - tile.startY);
            tile.image = cvImage(roi).clone();

            tiles.push_back(tile);

            // 如果已经到达图像右边界，跳出内层循环
            if (tile.endX >= imageWidth)
            {
                break;
            }
        }

        // 如果已经到达图像下边界，跳出外层循环
        if (tiles.back().endY >= imageHeight)
        {
            break;
        }
    }

    // qDebug() << "sliceImage() - Generated" << tiles.size() << "tiles";
    return tiles;
}

/**
 * @brief 合并切片检测结果
 */
trtyolo::DetectRes ProcessThread::mergeDetectionResults(const std::vector<trtyolo::DetectRes>& tileResults,
                                                        const std::vector<TileInfo>& tiles,
                                                        int imageWidth,
                                                        int imageHeight)
{
    trtyolo::DetectRes mergedResult;

    if (tileResults.size() != tiles.size())
    {
        qWarning() << "mergeDetectionResults() - Mismatch between tile results and tiles count";
        return mergedResult;
    }

    // 合并所有切片的检测结果
    for (size_t i = 0; i < tileResults.size(); ++i)
    {
        const auto& result = tileResults[i];
        const auto& tile = tiles[i];

        // 遍历当前切片的所有检测结果
        for (int j = 0; j < result.num; ++j)
        {
            // 将检测框坐标从切片坐标系映射到原图坐标系
            trtyolo::Box mappedBox(
                result.boxes[j].left + tile.startX,
                result.boxes[j].top + tile.startY,
                result.boxes[j].right + tile.startX,
                result.boxes[j].bottom + tile.startY
            );

            // 检查映射后的框是否在图像范围内
            if (mappedBox.left >= 0 && mappedBox.top >= 0 &&
                mappedBox.right <= imageWidth && mappedBox.bottom <= imageHeight)
            {
                mergedResult.boxes.push_back(mappedBox);
                mergedResult.classes.push_back(result.classes[j]);
                mergedResult.scores.push_back(result.scores[j]);
                mergedResult.num++;
            }
        }
    }

    // qDebug() << "mergeDetectionResults() - Merged" << mergedResult.num << "detections from" << tiles.size() << "tiles (before NMS)";

    // 应用NMS去除重复的检测框
    // IoU阈值设置为0.5，可以根据实际情况调整
    if (mergedResult.num > 0)
    {
        mergedResult = applyNMS(mergedResult, 0.5f);
    }

    return mergedResult;
}

/**
 * @brief 执行 YOLO 缺陷检测（支持切片）
 */
bool ProcessThread::detectDefects(const QImage& rgbImage, int tileSize, double overlapRatio, trtyolo::DetectRes& result)
{
    if (!m_mainWindow || !m_mainWindow->m_yoloDetector)
    {
        qWarning() << "detectDefects() - YOLO detector not initialized";
        return false;
    }

    if (rgbImage.isNull())
    {
        qWarning() << "detectDefects() - Input image is null";
        return false;
    }

    try
    {
        // 将 QImage 转换为 OpenCV Mat
        cv::Mat cvImage = qImageToMat(rgbImage);
        if (cvImage.empty())
        {
            qWarning() << "detectDefects() - Failed to convert QImage to Mat";
            return false;
        }

        // 确保图像是 BGR 格式（YOLO 模型需要）
        if (cvImage.channels() == 1)
        {
            cv::cvtColor(cvImage, cvImage, cv::COLOR_GRAY2BGR);
        }

        // 如果 tileSize 为 0 或图像小于切片尺寸，则直接检测整图
        if (tileSize <= 0 || (cvImage.cols <= tileSize && cvImage.rows <= tileSize))
        {
            qDebug() << "detectDefects() - Detecting on full image (no tiling)";

            // 创建 trtyolo::Image 对象
            trtyolo::Image yoloImage(cvImage.data, cvImage.cols, cvImage.rows);

            // 执行推理
            result = m_mainWindow->m_yoloDetector->predict(yoloImage);

            return true;
        }

        // 切片检测流程
        // 1. 切片
        std::vector<TileInfo> tiles = sliceImage(cvImage, tileSize, overlapRatio);
        if (tiles.empty())
        {
            qWarning() << "detectDefects() - Failed to slice image";
            return false;
        }

        // 2. 对每个切片进行检测
        std::vector<trtyolo::DetectRes> tileResults;
        tileResults.reserve(tiles.size());

        for (size_t i = 0; i < tiles.size(); ++i)
        {
            const auto& tile = tiles[i];

            // 创建 trtyolo::Image 对象
            trtyolo::Image yoloImage(tile.image.data, tile.image.cols, tile.image.rows);

            // 执行推理
            trtyolo::DetectRes tileResult = m_mainWindow->m_yoloDetector->predict(yoloImage);
            tileResults.push_back(tileResult);

            // if (tileResult.num > 0)
            // {
            //     qDebug() << "detectDefects() - Tile" << i << ": detected" << tileResult.num << "objects";
            // }
        }

        // 3. 合并检测结果
        result = mergeDetectionResults(tileResults, tiles, cvImage.cols, cvImage.rows);

        // qDebug() << "detectDefects() - Final result:" << result.num << "objects detected";
        return true;
    }
    catch (const std::exception& e)
    {
        qCritical() << "detectDefects() - Exception:" << e.what();
        return false;
    }
}

/**
 * @brief 在图像上绘制检测框
 */
QImage ProcessThread::drawDetectionBoxes(const QImage& image, const trtyolo::DetectRes& result)
{
    if (image.isNull())
    {
        qWarning() << "drawDetectionBoxes() - Input image is null";
        return QImage();
    }

    // 创建副本用于绘制
    QImage drawnImage = image.copy();

    if (result.num == 0)
    {
        return drawnImage;
    }

    // 使用 QPainter 绘制检测框
    QPainter painter(&drawnImage);
    painter.setRenderHint(QPainter::Antialiasing);

    // 定义颜色列表（最多支持10种类别）
    static const QColor colors[] = {
        QColor(255, 0, 0),     // 红色
        QColor(0, 255, 0),     // 绿色
        QColor(0, 0, 255),     // 蓝色
        QColor(255, 255, 0),   // 黄色
        QColor(255, 0, 255),   // 品红
        QColor(0, 255, 255),   // 青色
        QColor(255, 128, 0),   // 橙色
        QColor(128, 0, 255),   // 紫色
        QColor(0, 128, 255),   // 天蓝
        QColor(128, 255, 0)    // 黄绿
    };

    // 类别映射：0-Shaving, 1-Dust, 2-Oil
    static const QMap<int, QString> classNames = {
        {0, "Shaving"},
        {1, "Dust"},
        {2, "Oil"},
        {3, "Black"},
        {4, "Damage"},
        {5, "Pollution"}
    };

    // 类别中文名称映射
    static const QMap<int, QString> classNamesChinese = {
        {0, "刨花"},
        {1, "灰尘"},
        {2, "油污"},
        {3, "黑点"},
        {4, "破坏"},
        {5, "污染"}
    };

    // 像素到毫米的转换系数
    constexpr double PIXEL_TO_MM = 0.177;

    // 遍历所有检测结果
    for (int i = 0; i < result.num; ++i)
    {
        const auto& box = result.boxes[i];
        int classId = result.classes[i];
        float score = result.scores[i];

        // 选择颜色
        QColor color = colors[classId % 10];
        QPen pen(color, 3);
        painter.setPen(pen);

        // 绘制矩形框
        QRectF rect(box.left, box.top, box.right - box.left, box.bottom - box.top);
        painter.drawRect(rect);

        // 获取类别名称，如果不存在则使用默认值
        QString className = classNames.value(classId, QString("Class %1").arg(classId));

        // 绘制类别和置信度文本
        QString label = QString("%1: %2%").arg(className).arg(score * 100, 0, 'f', 1);

        // 设置文本背景
        QFont font = painter.font();
        font.setPointSize(10);
        font.setBold(true);
        painter.setFont(font);

        QFontMetrics fm(font);
        QRect textRect = fm.boundingRect(label);
        textRect.moveTopLeft(QPoint(box.left, box.top - textRect.height() - 2));

        // 绘制文本背景
        painter.fillRect(textRect, color);

        // 绘制文本
        painter.setPen(Qt::white);
        painter.drawText(textRect, Qt::AlignCenter, label);

        // 【添加缺陷信息到 DefectListWidget】
        // 计算缺陷的像素尺寸
        int widthPixels = box.right - box.left;
        int heightPixels = box.bottom - box.top;

        // 转换为毫米
        double widthMm = widthPixels * PIXEL_TO_MM;
        double heightMm = heightPixels * PIXEL_TO_MM;

        // 获取缺陷中文名称
        QString defectName = classNamesChinese.value(classId, QString("未知缺陷%1").arg(classId));

        // 格式化显示文本
        QString defectInfo = QString("%1 - 尺寸: %2 mm × %3 mm")
                                 .arg(defectName)
                                 .arg(widthMm, 0, 'f', 2)
                                 .arg(heightMm, 0, 'f', 2);

        // 使用信号发送到主线程更新UI（线程安全）
        emit addDefectInfo(defectInfo);
    }

    painter.end();
    return drawnImage;
}

/**
 * @brief 根据像素面积过滤检测结果
 */
trtyolo::DetectRes ProcessThread::filterDefectsByArea(const trtyolo::DetectRes& result, int minArea)
{
    if (minArea <= 0)
    {
        // 如果阈值无效，直接返回原结果
        return result;
    }

    trtyolo::DetectRes filteredResult;

    // 置信度阈值（默认0.3）
    const float minConfidence = 0.3f;

    // 遍历所有检测结果
    for (int i = 0; i < result.num; ++i)
    {
        const auto& box = result.boxes[i];
        float confidence = result.scores[i];

        // 第一步：置信度过滤
        if (confidence < minConfidence)
        {
            continue;
        }

        // 第二步：面积过滤
        // 计算检测框的像素面积
        int width = box.right - box.left;
        int height = box.bottom - box.top;
        int area = width * height;

        // 如果面积大于等于阈值，保留该检测框
        if (area >= minArea)
        {
            filteredResult.boxes.push_back(box);
            filteredResult.classes.push_back(result.classes[i]);
            filteredResult.scores.push_back(result.scores[i]);
            filteredResult.num++;
        }
        // else
        // {
        //     // 输出被过滤掉的缺陷信息（可选，用于调试）
        //     qDebug() << "ProcessThread::filterDefectsByArea() - Filtered out defect with area" << area
        //              << "(threshold:" << minArea << ")";
        // }
    }

    // qDebug() << "ProcessThread::filterDefectsByArea() - Filtered from" << result.num
    //          << "to" << filteredResult.num << "defects (threshold:" << minArea << "pixels)";

    return filteredResult;
}

/**
 * @brief 初始化PLC连接
 * @param ip PLC的IP地址
 * @param rack 机架号
 * @param slot 槽号
 * @return 成功返回true，失败返回false
 */
bool ProcessThread::initializePLC(const QString& ip, int rack, int slot)
{
    // 如果已经存在PLC管理器，先断开并删除
    if (m_plcManager)
    {
        disconnectPLC();
    }

    // 创建PLC管理器（在当前线程中）
    m_plcManager = new PLCManager();

    // 连接到PLC
    bool success = m_plcManager->connectToPLC(ip, rack, slot);

    if (success)
    {
        qDebug() << "ProcessThread::initializePLC() - PLC connected successfully:" << ip;
    }
    else
    {
        QString errorMsg = m_plcManager->getLastError();
        qWarning() << "ProcessThread::initializePLC() - PLC connection failed:" << errorMsg;

        // 连接失败，清理资源
        delete m_plcManager;
        m_plcManager = nullptr;
    }

    return success;
}

/**
 * @brief 断开PLC连接
 */
void ProcessThread::disconnectPLC()
{
    if (m_plcManager)
    {
        m_plcManager->disconnectFromPLC();
        delete m_plcManager;
        m_plcManager = nullptr;

        qDebug() << "ProcessThread::disconnectPLC() - PLC disconnected";
    }
}

/**
 * @brief 生成每日CSV文件名
 * @return 格式化的CSV文件名 (例: defects_2025-11-13.csv)
 */
QString ProcessThread::generateCSVFilename()
{
    const QDateTime currentDateTime = QDateTime::currentDateTime();
    const QString dateString = currentDateTime.toString("yyyy-MM-dd");
    return QString("defects_%1.csv").arg(dateString);
}

/**
 * @brief 保存缺陷信息到CSV文件（静态方法，支持异步调用）
 * @param imageName 图片文件名
 * @param detectResult 检测结果
 * @param basePath 保存路径
 * @return true-成功, false-失败
 */
bool ProcessThread::saveDefectsToCSV(const QString& imageName, const trtyolo::DetectRes& detectResult, const QString& basePath)
{
    if (detectResult.num == 0)
    {
        // 没有缺陷，不需要保存
        return true;
    }

    if (basePath.isEmpty())
    {
        qWarning() << "saveDefectsToCSV() - Base path is empty";
        return false;
    }

    // 生成CSV文件路径
    const QString csvFilename = generateCSVFilename();
    const QString csvPath = QDir(basePath).filePath(csvFilename);

    // 检查文件是否存在，决定是否需要写入表头
    bool fileExists = QFile::exists(csvPath);

    // 打开文件（追加模式）
    QFile csvFile(csvPath);
    if (!csvFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        qCritical() << "saveDefectsToCSV() - Failed to open CSV file:" << csvPath;
        return false;
    }

    QTextStream out(&csvFile);
    out.setEncoding(QStringConverter::Utf8);  // 设置UTF-8编码

    // 如果是新文件，写入表头
    if (!fileExists)
    {
        out << "Timestamp,ImageName,DefectClass,ClassName,Left,Top,Right,Bottom,Confidence\n";
        qDebug() << "saveDefectsToCSV() - Created new CSV file with header:" << csvPath;
    }

    // 获取当前时间戳
    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");

    // 类别映射：0-Shaving, 1-Dust, 2-Oil
    static const QMap<int, QString> classNames = {
      {0, "Shaving"},
      {1, "Dust"},
      {2, "Oil"},
      {3, "Black"},
      {4, "Damage"},
      {5, "Pollution"}
    };

    // 写入每个缺陷的记录
    for (int i = 0; i < detectResult.num; ++i)
    {
        const auto& box = detectResult.boxes[i];
        int classId = detectResult.classes[i];
        float confidence = detectResult.scores[i];

        // 获取类别名称
        QString className = classNames.value(classId, QString("Unknown_%1").arg(classId));

        // 写入CSV行
        // 格式: Timestamp,ImageName,DefectClass,ClassName,Left,Top,Right,Bottom,Confidence
        out << timestamp << ","
            << imageName << ","
            << classId << ","
            << className << ","
            << box.left << ","
            << box.top << ","
            << box.right << ","
            << box.bottom << ","
            << QString::number(confidence, 'f', 4) << "\n";
    }

    csvFile.close();

    qDebug() << "saveDefectsToCSV() - Saved" << detectResult.num << "defect(s) to" << csvPath;
    return true;
}

/**
 * @brief 计算两个检测框的IoU（交并比）
 */
float ProcessThread::calculateIoU(const trtyolo::Box& box1, const trtyolo::Box& box2)
{
    // 计算交集区域的坐标
    float intersectLeft = std::max(box1.left, box2.left);
    float intersectTop = std::max(box1.top, box2.top);
    float intersectRight = std::min(box1.right, box2.right);
    float intersectBottom = std::min(box1.bottom, box2.bottom);

    // 如果没有交集，返回0
    if (intersectLeft >= intersectRight || intersectTop >= intersectBottom)
    {
        return 0.0f;
    }

    // 计算交集面积
    float intersectArea = (intersectRight - intersectLeft) * (intersectBottom - intersectTop);

    // 计算两个框的面积
    float box1Area = (box1.right - box1.left) * (box1.bottom - box1.top);
    float box2Area = (box2.right - box2.left) * (box2.bottom - box2.top);

    // 计算并集面积
    float unionArea = box1Area + box2Area - intersectArea;

    // 避免除以0
    if (unionArea <= 0.0f)
    {
        return 0.0f;
    }

    // 返回IoU
    return intersectArea / unionArea;
}

/**
 * @brief 非极大值抑制（NMS）算法，去除重复的检测框
 */
trtyolo::DetectRes ProcessThread::applyNMS(const trtyolo::DetectRes& result, float iouThreshold)
{
    if (result.num == 0)
    {
        return result;
    }

    // 创建索引数组，用于排序
    std::vector<int> indices(result.num);
    for (int i = 0; i < result.num; ++i)
    {
        indices[i] = i;
    }

    // 按置信度降序排序
    std::sort(indices.begin(), indices.end(), [&result](int i1, int i2) {
        return result.scores[i1] > result.scores[i2];
    });

    // 标记被抑制的检测框
    std::vector<bool> suppressed(result.num, false);

    // NMS主循环
    for (size_t i = 0; i < indices.size(); ++i)
    {
        int idx1 = indices[i];
        if (suppressed[idx1])
        {
            continue;
        }

        // 与后续所有框比较
        for (size_t j = i + 1; j < indices.size(); ++j)
        {
            int idx2 = indices[j];
            if (suppressed[idx2])
            {
                continue;
            }

            // 只对同一类别的检测框应用NMS
            if (result.classes[idx1] != result.classes[idx2])
            {
                continue;
            }

            // 计算IoU
            float iou = calculateIoU(result.boxes[idx1], result.boxes[idx2]);

            // 如果IoU大于阈值，抑制置信度较低的框
            if (iou > iouThreshold)
            {
                suppressed[idx2] = true;
            }
        }
    }

    // 构建NMS后的结果
    trtyolo::DetectRes nmsResult;
    for (int i = 0; i < result.num; ++i)
    {
        if (!suppressed[i])
        {
            nmsResult.boxes.push_back(result.boxes[i]);
            nmsResult.classes.push_back(result.classes[i]);
            nmsResult.scores.push_back(result.scores[i]);
            nmsResult.num++;
        }
    }

    qDebug() << "ProcessThread::applyNMS() - NMS filtered from" << result.num
             << "to" << nmsResult.num << "detections (IoU threshold:" << iouThreshold << ")";

    return nmsResult;
}
