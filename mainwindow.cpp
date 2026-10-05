#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIntValidator>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QStyle>
#include "branding.h"

// 常量定义
namespace {
    constexpr int MAX_QUEUE_COUNT = 50;  // 队列缓冲区个数
}

// ==================== UI 控制辅助函数 ====================

/**
 * @brief 设置设备控制按钮使能状态
 * @param enable true-设备已打开, false-设备未打开
 */
void MainWindow::setDeviceControlsEnabled(bool enable)
{
    ui->DeviceOpen->setEnabled(!enable);
    ui->DeviceClose->setEnabled(enable);
    ui->StartGrab->setEnabled(enable);
    ui->SearchDevice->setEnabled(!enable);
    setStatusChip(ui->CamStatusLabel, enable ? "已打开" : "未打开", enable ? "ok" : "off");
}

void MainWindow::setStatusChip(QLabel* label, const QString& text, const char* state)
{
    label->setText(text);
    label->setProperty("state", state);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

void MainWindow::updateModelStatus()
{
    const bool loaded = m_yoloDetector != nullptr;
    setStatusChip(ui->ModelStatusLabel, loaded ? "已加载" : "未加载", loaded ? "ok" : "error");
}

/**
 * @brief 设置参数控制控件使能状态
 * @param enable true-启用, false-禁用
 */
void MainWindow::setParameterControlsEnabled(bool enable)
{
    ui->ExposureEdit->setEnabled(enable);
    ui->WidthEdit->setEnabled(enable);
    ui->HeightEdit->setEnabled(enable);
    ui->PreampGainBox->setEnabled(enable);
    ui->DigitalGainEdit->setEnabled(enable);
    // AcquisitionLineRateEdit的使能状态由AcquisitionLineRateEnableBox控制
    ui->AcquisitionLineRateEdit->setEnabled(enable && ui->AcquisitionLineRateEnableBox->isChecked());
    ui->AcquisitionLineRateEnableBox->setEnabled(enable);
    ui->ResultLineRateEdit->setEnabled(enable);
    ui->ResultFrameRateEdit->setEnabled(enable);
    ui->GetPara->setEnabled(enable);
    ui->SetPara->setEnabled(enable);
    ui->imgSavePathEdit->setEnabled(enable);
    ui->AcquisitionBurstFrameCountEdit->setEnabled(enable);
    ui->SavePara->setEnabled(enable);
    ui->LoadPara->setEnabled(enable);
}

/**
 * @brief 设置采集控制控件使能状态
 * @param enable true-启用, false-禁用
 */
void MainWindow::setAcquisitionControlsEnabled(bool enable)
{
    ui->TriggerSelectBox->setEnabled(enable);
    ui->TriggerModeBox->setEnabled(enable);
    ui->TriggerSourceBox->setEnabled(enable);
    ui->PixelFormatBox->setEnabled(enable);
    ui->HBFormatBox->setEnabled(enable);
}

/**
 * @brief 设置采集过程中的控件使能状态
 * @param isGrabbing true-正在采集, false-未采集
 */
void MainWindow::setGrabbingControlsEnabled(bool isGrabbing)
{
    ui->StartGrab->setEnabled(!isGrabbing);
    ui->StopGrab->setEnabled(isGrabbing);

    // 采集时禁用这些控件：处理线程使用开始采集时的参数快照，期间修改不会生效
    setParameterControlsEnabled(!isGrabbing);
    setAcquisitionControlsEnabled(!isGrabbing);
    ui->SelectSavePath->setEnabled(!isGrabbing);
    ui->SelectModelPath->setEnabled(!isGrabbing);
    ui->DetectCheckBox->setEnabled(!isGrabbing && m_yoloDetector);
    ui->TileSizeComboBox->setEnabled(!isGrabbing);
    ui->OverlapRatioCombobox->setEnabled(!isGrabbing);
    ui->DefectFilterBox->setEnabled(!isGrabbing);
    ui->FilterSizeCombobox->setEnabled(!isGrabbing);
    ui->SaveDefectCheckBox->setEnabled(!isGrabbing);
    ui->IPEdit->setEnabled(!isGrabbing);
    ui->RackEdit->setEnabled(!isGrabbing);
    ui->SlotEdit->setEnabled(!isGrabbing);
    setStatusChip(ui->CamStatusLabel, isGrabbing ? "采集中" : "已打开", isGrabbing ? "busy" : "ok");
}

/**
 * @brief 读取相机枚举参数，把全部选项填入下拉框并选中当前值
 * @param map 非空时记录 选项名 → 枚举值
 */
int MainWindow::fillEnumCombo(const char* key, QComboBox* box, std::map<QString, int>* map)
{
    MVCC_ENUMVALUE value = {};
    int nRet = m_MyCamera->GetEnumValue(key, &value);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    box->clear();
    for (unsigned int i = 0; i < value.nSupportedNum; i++)
    {
        MVCC_ENUMENTRY entry = {};
        entry.nValue = value.nSupportValue[i];
        m_MyCamera->GetEnumEntrySymbolic(key, &entry);

        const QString symbolic = QString::fromLatin1(entry.chSymbolic);
        box->addItem(symbolic);
        if (map)
        {
            (*map)[symbolic] = int(entry.nValue);
        }
        if (value.nCurValue == value.nSupportValue[i])
        {
            box->setCurrentIndex(int(i));
        }
    }
    return MV_OK;
}

void MainWindow::fillSerialPorts(QComboBox* box)
{
    box->clear();
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& info : ports)
    {
        box->addItem(info.portName());
    }
    if (ports.isEmpty())
    {
        box->setCurrentText("未检测到可用串口");
    }
}

/**
 * @brief 加载 TensorRT 检测模型，替换当前模型（调用方须保证未在采集）
 */
bool MainWindow::loadModel(const QString& path, QString* error)
{
    m_modelPath = path;
    if (!QFileInfo::exists(path))
    {
        m_yoloDetector.reset();
        *error = "模型文件不存在";
        return false;
    }

    try
    {
        // 模型输入为 RGB，处理线程直接传入 RGB888 图像，不需要通道交换
        trtyolo::InferOption option;
        m_yoloDetector = std::make_unique<trtyolo::DetectModel>(path.toStdString(), option);
        qDebug() << "YOLO 检测器加载成功，模型路径:" << path;
        return true;
    }
    catch (const std::exception& e)
    {
        m_yoloDetector.reset();
        *error = QString::fromLocal8Bit(e.what());
        qCritical() << "YOLO 检测器加载失败:" << e.what();
        return false;
    }
}

ProcessConfig MainWindow::makeProcessConfig() const
{
    ProcessConfig config;
    config.framesPerBoard = ui->AcquisitionBurstFrameCountEdit->text().toInt();
    config.savePath = ui->imgSavePathEdit->text();
    config.hbDecode = ui->HBFormatBox->currentText() == "HB";
    config.detect = m_yoloDetector && ui->DetectCheckBox->isChecked();
    // 切片尺寸 "1024×1024" → 1024；重叠比例 "20%" → 0.2
    config.tileSize = ui->TileSizeComboBox->currentText().split(QRegularExpression("[×xX]")).value(0).toInt();
    config.overlapRatio = QString(ui->OverlapRatioCombobox->currentText()).remove('%').toDouble() / 100.0;
    config.filterByArea = ui->DefectFilterBox->isChecked();
    config.minArea = ui->FilterSizeCombobox->currentText().toInt();
    config.saveCsv = ui->SaveDefectCheckBox->isChecked();
    config.plcIp = ui->IPEdit->text().trimmed();
    config.plcRack = ui->RackEdit->text().toInt();
    config.plcSlot = ui->SlotEdit->text().toInt();
    config.displaySize = ui->PicLabel->size() * ui->PicLabel->devicePixelRatioF();
    return config;
}

// ==================== 主程序代码 ====================

/**
 * @brief 构造函数
 * 初始化主窗口和所有成员变量
 */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , initialflag(false)
    , m_TriggerModeCheck(false)
    , m_bAcquisitionLineRate(false)
    , m_HBMode(false)
    , m_lightController(std::make_unique<HikLightController>())
    , m_conveyorController(new ConveyorController(this))
{
    ui->setupUi(this);

    // 窗口标题来自 :/branding/branding.ini
    setWindowTitle(Branding::instance().windowTitle);

    // 初始化UI控件状态（设备未打开）
    setDeviceControlsEnabled(false);
    setParameterControlsEnabled(false);
    setAcquisitionControlsEnabled(false);
    ui->StopGrab->setEnabled(false);
    ui->SingleSoftTrigger->setEnabled(false);

    // 加载 YOLO 检测器，成功则默认启用检测
    QString error;
    const bool modelLoaded = loadModel(ui->ModelPathEdit->text(), &error);
    if (!modelLoaded)
    {
        qWarning() << "YOLO 检测器未加载:" << m_modelPath << error;
    }
    ui->DetectCheckBox->setEnabled(modelLoaded);
    ui->DetectCheckBox->setChecked(modelLoaded);
    updateModelStatus();

    // 缺陷数随列表变化（每行一个缺陷，每块板开始时清空）。
    // 只捕获模型和标签本身、以标签为接收者：窗口析构时 ui 已释放，而 QListWidget 析构仍会发 modelReset
    QAbstractItemModel* defectModel = ui->DefectListWidget->model();
    QLabel* defectCountLabel = ui->DefectCountLabel;
    auto updateDefectCount = [defectModel, defectCountLabel]() {
        defectCountLabel->setText(QString::number(defectModel->rowCount()));
    };
    connect(defectModel, &QAbstractItemModel::rowsInserted, defectCountLabel, updateDefectCount);
    connect(defectModel, &QAbstractItemModel::rowsRemoved, defectCountLabel, updateDefectCount);
    connect(defectModel, &QAbstractItemModel::modelReset, defectCountLabel, updateDefectCount);

    // ==================== 光源控制器 ====================
    fillSerialPorts(ui->PortCombobox);
    updateLightControlsEnabled(false);
    ui->LightnessEdit->setValidator(new QIntValidator(0, 255, this));
    setStatusChip(ui->LightStatusLabel, "未连接", "off");

    // ==================== 传送带控制器 ====================
    connect(m_conveyorController, &ConveyorController::statusUpdated,
            this, &MainWindow::updateConveyorStatus);
    // 串口异常断开（如拔线）时恢复按钮状态
    connect(m_conveyorController, &ConveyorController::portClosed,
            this, [this]() { updateConveyorControlsEnabled(false); });
    fillSerialPorts(ui->ConveyorPortComboBox);
    updateConveyorControlsEnabled(false);
    setStatusChip(ui->ConSatus, "未连接", "off");

    // 自动搜索设备
    on_SearchDevice_clicked();
}

MainWindow::~MainWindow()
{
    // 先停采集线程再关相机
    on_DeviceClose_clicked();
    // 关串口会发出 statusUpdated 回调本窗口，必须在 ui 释放前完成；光源控制器析构时自行关串口
    m_conveyorController->closeSerial();
    delete ui;
}

int MainWindow::GetTriggerSelector()
{
    return fillEnumCombo("TriggerSelector", ui->TriggerSelectBox);
}

int MainWindow::GetTriggerMode()
{
    return fillEnumCombo("TriggerMode", ui->TriggerModeBox);
}

int MainWindow::GetExposureTime()
{
    MVCC_FLOATVALUE stFloatValue = {0};

    int nRet = m_MyCamera->GetFloatValue("ExposureTime", &stFloatValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    double m_ExposureEdit = stFloatValue.fCurValue;
    ui->ExposureEdit->setText(QString::number(m_ExposureEdit));

    return MV_OK;
}

int MainWindow::SetExposureTime()
{
    m_MyCamera->SetEnumValue("ExposureAuto", MV_EXPOSURE_AUTO_MODE_OFF);

    return m_MyCamera->SetFloatValue("ExposureTime", ui->ExposureEdit->text().toFloat());
}

int MainWindow::GetPreampGain()
{
    return fillEnumCombo("PreampGain", ui->PreampGainBox, &m_mapPreampGain);
}

int MainWindow::GetDigitalGain()
{
    MVCC_FLOATVALUE stFloatValue = {0};

    int nRet = m_MyCamera->GetFloatValue("DigitalShift", &stFloatValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    double digitalGain = stFloatValue.fCurValue;
    ui->DigitalGainEdit->setText(QString::number(digitalGain));

    return MV_OK;
}

int MainWindow::SetDigitalGain()
{
    // 设置增益前先把增益使能开关打开，失败无需返回
    m_MyCamera->SetBoolValue("DigitalShiftEnable", true);

    return m_MyCamera->SetFloatValue("DigitalShift", ui->DigitalGainEdit->text().toFloat());
}

int MainWindow::GetAcquisitionLineRateEnable()
{
    bool bAcquisitionLineRateEnable = false;
    int nRet = m_MyCamera->GetBoolValue("AcquisitionLineRateEnable", &bAcquisitionLineRateEnable);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    if (true == bAcquisitionLineRateEnable)
    {
        ui->AcquisitionLineRateEnableBox->setChecked(true);
    }
    else
    {
        ui->AcquisitionLineRateEnableBox->setChecked(false);
    }

    return MV_OK;
}

int MainWindow::GetAcquisitionLineRate()
{
    MVCC_INTVALUE_EX stIntValue = { 0 };

    int nRet = m_MyCamera->GetIntValue("AcquisitionLineRate", &stIntValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }
    double AcquisitionLineRate = stIntValue.nCurValue;
    ui->AcquisitionLineRateEdit->setText(QString::number(AcquisitionLineRate));
    m_bAcquisitionLineRate = true;

    return MV_OK;
}

int MainWindow::GetResultingLineRate()
{
    MVCC_INTVALUE_EX stIntValue = { 0 };

    int nRet = m_MyCamera->GetIntValue("ResultingLineRate", &stIntValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }
    double ResultingLineRate = stIntValue.nCurValue;
    ui->ResultLineRateEdit->setText(QString::number(ResultingLineRate));

    return MV_OK;
}

int MainWindow::SetAcquisitionLineRate()
{
    return m_MyCamera->SetIntValue("AcquisitionLineRate", ui->AcquisitionLineRateEdit->text().toInt());
}

int MainWindow::GetResultingFrameRate()
{
    MVCC_FLOATVALUE stFloatValue = { 0 };

    int nRet = m_MyCamera->GetFloatValue("ResultingFrameRate", &stFloatValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }
    double ResultingFrameRate = stFloatValue.fCurValue;
    ui->ResultFrameRateEdit->setText(QString::number(ResultingFrameRate));

    return MV_OK;
}

int MainWindow::GetImageCompressionMode()
{
    const int nRet = fillEnumCombo("ImageCompressionMode", ui->HBFormatBox);
    if (MV_OK == nRet)
    {
        m_HBMode = true;
    }
    return nRet;
}

int MainWindow::GetTriggerSource()
{
    const int nRet = fillEnumCombo("TriggerSource", ui->TriggerSourceBox, &m_mapTriggerSource);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    if (ui->TriggerSelectBox->currentText() == "FrameBurstStart" && ui->TriggerModeBox->currentText() == "On" &&
        ui->TriggerSourceBox->currentText() == "Software")
    {
        m_TriggerModeCheck = true;
    }
    return MV_OK;
}

int MainWindow::GetPixelFormat()
{
    return fillEnumCombo("PixelFormat", ui->PixelFormatBox, &m_mapPixelFormat);
}

int MainWindow::GetWidthHeight()
{
    MVCC_INTVALUE_EX stIntValue = { 0 };

    int nRet = m_MyCamera->GetIntValue("Width", &stIntValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    double Width = stIntValue.nCurValue;
    ui->WidthEdit->setText(QString::number(Width));

    nRet = m_MyCamera->GetIntValue("Height", &stIntValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    double Height = stIntValue.nCurValue;
    ui->HeightEdit->setText(QString::number(Height));

    return MV_OK;

}

int MainWindow::SetWidthHeight()
{
    int nRet =  m_MyCamera->SetIntValue("Width", ui->WidthEdit->text().toInt());
    if (MV_OK != nRet)
    {
        return nRet;
    }

    nRet =  m_MyCamera->SetIntValue("Height", ui->HeightEdit->text().toInt());
    if (MV_OK != nRet)
    {
        return nRet;
    }
    return MV_OK;
}

int MainWindow::GetAcquisitionBurstFrameCount()
{
    MVCC_INTVALUE_EX stIntValue = { 0 };

    int nRet = m_MyCamera->GetIntValue("AcquisitionBurstFrameCount", &stIntValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    double temp = stIntValue.nCurValue;
    ui->AcquisitionBurstFrameCountEdit->setText(QString::number(temp));

    return MV_OK;
}

int MainWindow::SetAcquisitionBurstFrameCount()
{
    int nRet =  m_MyCamera->SetIntValue("AcquisitionBurstFrameCount", ui->AcquisitionBurstFrameCountEdit->text().toInt());
    if (MV_OK != nRet)
    {
        return nRet;
    }
    return MV_OK;
}

void __stdcall MainWindow::ImageCallBack(unsigned char * pData, MV_FRAME_OUT_INFO_EX* pFrameInfo, void* pUser)
{
    if (nullptr == pFrameInfo || nullptr == pData)
    {
        return;
    }

    auto* pThis = static_cast<MainWindow*>(pUser);
    const int nRet = pThis->m_queue->push(*pFrameInfo, pData);
    if (ArrayQueue::E_BUFOVER == nRet)
    {
        qWarning() << "Image queue full, dropping frame" << pFrameInfo->nFrameNum;
    }
}

void MainWindow::on_SearchDevice_clicked()
{
    ui->DeviceStrBox->clear();
    memset(&m_stDevList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));

    // 枚举子网内所有设备
    int nRet = CMvCamera::EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE | MV_GENTL_CAMERALINK_DEVICE ,&m_stDevList);

    if (MV_OK != nRet)
    {
        return;
    }

    if (m_stDevList.nDeviceNum == 0)
    {
        QMessageBox::critical(this,"device","No device!");
        return;
    }

    // 列出全部设备，条目数据保存其在 m_stDevList 中的下标
    for (unsigned int i = 0; i < m_stDevList.nDeviceNum; i++)
    {
        const MV_CC_DEVICE_INFO* pDeviceInfo = m_stDevList.pDeviceInfo[i];
        if (nullptr == pDeviceInfo)
        {
            continue;
        }

        QString deviceStr;
        if (pDeviceInfo->nTLayerType == MV_GIGE_DEVICE)
        {
            const MV_GIGE_DEVICE_INFO& gige = pDeviceInfo->SpecialInfo.stGigEInfo;
            const unsigned int ip = gige.nCurrentIp;
            deviceStr = QString("[%1]GigE: %2 %3 (%4)  %5.%6.%7.%8")
                            .arg(i)
                            .arg(QString::fromLocal8Bit(reinterpret_cast<const char*>(gige.chManufacturerName)),
                                 QString::fromLocal8Bit(reinterpret_cast<const char*>(gige.chModelName)),
                                 QString::fromLocal8Bit(reinterpret_cast<const char*>(gige.chSerialNumber)))
                            .arg((ip >> 24) & 0xff).arg((ip >> 16) & 0xff).arg((ip >> 8) & 0xff).arg(ip & 0xff);
        }
        else if (pDeviceInfo->nTLayerType == MV_USB_DEVICE)
        {
            const MV_USB3_DEVICE_INFO& usb = pDeviceInfo->SpecialInfo.stUsb3VInfo;
            deviceStr = QString("[%1]USB: %2 %3 (%4)")
                            .arg(i)
                            .arg(QString::fromLocal8Bit(reinterpret_cast<const char*>(usb.chManufacturerName)),
                                 QString::fromLocal8Bit(reinterpret_cast<const char*>(usb.chModelName)),
                                 QString::fromLocal8Bit(reinterpret_cast<const char*>(usb.chSerialNumber)));
        }
        else
        {
            deviceStr = QString("[%1]Device type 0x%2").arg(i).arg(pDeviceInfo->nTLayerType, 0, 16);
        }
        ui->DeviceStrBox->addItem(deviceStr, int(i));
    }

    ui->DeviceStrBox->setCurrentIndex(0);
    ui->DeviceOpen->setEnabled(ui->DeviceStrBox->count() > 0);
}

void MainWindow::on_DeviceOpen_clicked()
{
    if (m_MyCamera)
    {
        return;
    }

    // 条目数据是设备在 m_stDevList 中的下标（下拉框序号与设备序号不一定相同）
    bool ok = false;
    const int nIndex = ui->DeviceStrBox->currentData().toInt(&ok);
    if (!ok || nIndex < 0 || nIndex >= int(m_stDevList.nDeviceNum) || nullptr == m_stDevList.pDeviceInfo[nIndex])
    {
        QMessageBox::critical(this,"error","Please select device!");
        return;
    }

    // 创建相机实例并打开
    m_MyCamera = std::make_unique<CMvCamera>();
    int nRet = m_MyCamera->Open(m_stDevList.pDeviceInfo[nIndex]);
    if (MV_OK != nRet)
    {
        m_MyCamera.reset();
        QMessageBox::critical(this,"error","Open Fail!");
        return;
    }

    // 探测网络最佳包大小(只对GigE相机有效)
    if (m_stDevList.pDeviceInfo[nIndex]->nTLayerType == MV_GIGE_DEVICE)
    {
        unsigned int nPacketSize = 0;
        nRet = m_MyCamera->GetOptimalPacketSize(&nPacketSize);
        if (nRet == MV_OK)
        {
            nRet = m_MyCamera->SetIntValue("GevSCPSPacketSize",nPacketSize);
            if(nRet != MV_OK)
            {
                QMessageBox::critical(this,"error","Warning: Set Packet Size fail!");
            }
        }
        else
        {
            QMessageBox::critical(this,"error","Warning: Get Packet Size fail!");
        }
    }


    // 获取所有参数
    on_GetPara_clicked();

    initialflag = true;

    // 设置曝光时间
    nRet = SetExposureTime();

    // 设备打开后启用所有控件
    setDeviceControlsEnabled(true);
    setParameterControlsEnabled(true);
    setAcquisitionControlsEnabled(true);
}

/**
 * @brief 关闭设备
 * 先停止采集，再释放相机资源，清理状态，禁用所有控件
 */
void MainWindow::on_DeviceClose_clicked()
{
    stopGrabbing();

    // 清理映射表和状态标志
    m_mapPixelFormat.clear();
    m_mapPreampGain.clear();
    m_mapTriggerSource.clear();
    m_TriggerModeCheck = false;
    m_bAcquisitionLineRate = false;
    m_HBMode = false;

    // 关闭并释放相机
    if (m_MyCamera)
    {
        m_MyCamera->Close();
        m_MyCamera.reset();
    }

    // 禁用所有控件
    setDeviceControlsEnabled(false);
    setParameterControlsEnabled(false);
    setAcquisitionControlsEnabled(false);
}

/**
 * @brief 获取相机参数
 * 从相机读取所有当前参数并更新到界面
 */
void MainWindow::on_GetPara_clicked()
{
    // 暂时禁用事件处理，避免获取参数时触发设置操作
    initialflag = false;

    int nRet = GetTriggerSelector();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Trigger Selector Fail!");
    }

    nRet = GetTriggerMode();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Trigger Mode Fail!");
    }

    nRet = GetTriggerSource();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Trigger Source Fail!");
    }

    nRet = GetExposureTime();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Exposure time Fail!");
    }

    nRet = GetPreampGain();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Preamp Gain Fail!");
    }

    nRet = GetDigitalGain();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Digital Gain Fail!");
    }

    nRet = GetAcquisitionLineRateEnable();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get AcquisitionLineRateEnable Fail!");
    }

    nRet = GetAcquisitionLineRate();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get AcquisitionLineRate Fail!");
    }

    nRet = GetResultingLineRate();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get ResultingLineRate Fail!");
    }

    nRet = GetResultingFrameRate();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get ResultingFrameRate Fail!");
    }

    nRet = GetPixelFormat();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Pixel Format Fail!");
    }

    nRet = GetImageCompressionMode();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get ImageCompressionMode Fail!");
    }

    nRet = GetWidthHeight();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get WidthHeight Fail!");
    }

    nRet = GetAcquisitionBurstFrameCount();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get AcquisitionBurstFrameCount Fail!");
    }

    // 参数获取完成，重新启用事件处理
    initialflag = true;
}

/**
 * @brief 设置相机参数
 * 将界面上的参数设置到相机
 */
void MainWindow::on_SetPara_clicked()
{
    bool allSucceeded = true;

    int nRet = SetExposureTime();
    if (nRet != MV_OK)
    {
        allSucceeded = false;
        QMessageBox::critical(this, "error", "Set Exposure Time Fail!");
    }

    nRet = SetDigitalGain();
    if (nRet != MV_OK)
    {
        allSucceeded = false;
        QMessageBox::critical(this, "error", "Set DigitalGain Fail!");
    }

    nRet = SetWidthHeight();
    if (nRet != MV_OK)
    {
        allSucceeded = false;
        QMessageBox::critical(this, "error", "Set WidthHeight Fail!");
    }

    nRet = SetAcquisitionBurstFrameCount();
    if (nRet != MV_OK)
    {
        allSucceeded = false;
        QMessageBox::critical(this, "error", "Set AcquisitionBurstFrameCount Fail!");
    }

    // 如果启用了行频控制，设置行频参数
    if (m_bAcquisitionLineRate)
    {
        nRet = SetAcquisitionLineRate();
        if (nRet != MV_OK)
        {
            allSucceeded = false;
            QMessageBox::critical(this, "error", "Set AcquisitionLineRate Fail!");
        }
    }

    if (allSucceeded)
    {
        QMessageBox::information(this, "success", "Set Parameter Succeed!");
    }
}

void MainWindow::on_StartGrab_clicked()
{
    if (!m_MyCamera || m_ProcessThread)
    {
        return;
    }

    ui->PicLabel->clear();

    // 队列每个缓冲区的大小取相机的 PayloadSize
    MVCC_INTVALUE_EX stIntEx = {0};
    int nRet = m_MyCamera->GetIntValue("PayloadSize", &stIntEx);
    const uint64_t imageSize = (MV_OK == nRet)
        ? uint64_t(stIntEx.nCurValue)
        : uint64_t(ui->WidthEdit->text().toInt()) * ui->HeightEdit->text().toInt() * 3;

    m_queue = std::make_unique<ArrayQueue>();
    if (ArrayQueue::OK != m_queue->init(MAX_QUEUE_COUNT, imageSize))
    {
        m_queue.reset();
        QMessageBox::critical(this, "error", "ArrayQueue init fail!");
        return;
    }

    // 注册图像回调函数
    nRet = m_MyCamera->RegisterImageCallBack(ImageCallBack, this);
    if (MV_OK != nRet)
    {
        m_queue.reset();
        QMessageBox::critical(this, "error", "Register callback function failed!");
        return;
    }

    // 开始采集
    nRet = m_MyCamera->StartGrabbing();
    if (MV_OK != nRet)
    {
        m_MyCamera->RegisterImageCallBack(nullptr, nullptr);
        m_queue.reset();
        QMessageBox::critical(this, "error", "Start grabbing fail!");
        return;
    }

    // 刷新参数显示，再按界面当前值生成处理参数快照（帧在此期间已进入队列）
    on_GetPara_clicked();

    m_ProcessThread = std::make_unique<ProcessThread>(*m_queue, *m_MyCamera, m_yoloDetector.get(),
                                                      makeProcessConfig());
    connect(m_ProcessThread.get(), &ProcessThread::frameReady, this, [this](const QImage& image) {
        const qreal dpr = ui->PicLabel->devicePixelRatioF();
        const QSize target = ui->PicLabel->size() * dpr;
        QPixmap pixmap = QPixmap::fromImage(image);
        if (pixmap.width() > target.width() || pixmap.height() > target.height())
        {
            pixmap = pixmap.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        pixmap.setDevicePixelRatio(dpr);
        ui->PicLabel->setPixmap(pixmap);
    });
    connect(m_ProcessThread.get(), &ProcessThread::boardStarted, ui->DefectListWidget, &QListWidget::clear);
    connect(m_ProcessThread.get(), &ProcessThread::defectsFound, ui->DefectListWidget, &QListWidget::addItems);
    connect(m_ProcessThread.get(), &ProcessThread::detectionTimeUpdated, this, &MainWindow::updateDetectionTime);
    m_ProcessThread->start();

    // 如果是软触发模式，启用软触发按钮
    ui->SingleSoftTrigger->setEnabled(ui->TriggerSourceBox->currentText() == "Software" && m_TriggerModeCheck);

    // 采集中，禁用大部分控件
    setGrabbingControlsEnabled(true);
}

/**
 * @brief 停止采集：停相机 → 注销回调 → 停处理线程 → 释放队列
 */
void MainWindow::stopGrabbing()
{
    if (!m_ProcessThread && !m_queue)
    {
        return;
    }

    if (m_MyCamera)
    {
        int nRet = m_MyCamera->StopGrabbing();
        if (MV_OK != nRet)
        {
            qWarning() << "StopGrabbing failed, error code:" << Qt::hex << nRet;
        }
        nRet = m_MyCamera->RegisterImageCallBack(nullptr, nullptr);
        if (MV_OK != nRet)
        {
            qWarning() << "Unregister image callback failed, error code:" << Qt::hex << nRet;
        }
    }

    m_ProcessThread.reset();  // 析构时请求中断并等待线程退出
    m_queue.reset();

    setGrabbingControlsEnabled(false);
    ui->SingleSoftTrigger->setEnabled(false);
}

/**
 * @brief 停止采集
 */
void MainWindow::on_StopGrab_clicked()
{
    if (!m_ProcessThread)
    {
        return;
    }

    stopGrabbing();

    // 刷新参数显示
    on_GetPara_clicked();
}

/**
 * @brief 执行单次软触发
 * 在软触发模式下，手动触发一次图像采集
 */
void MainWindow::on_SingleSoftTrigger_clicked()
{
    if (!m_ProcessThread)
    {
        return;
    }

    m_MyCamera->CommandExecute("TriggerSoftware");
}

/**
 * @brief 触发选项切换处理
 * 设置触发选项为帧突发开始或行开始，并更新软触发按钮状态
 */
void MainWindow::on_TriggerSelectBox_currentIndexChanged()
{
    if (!initialflag)
    {
        return;
    }

    QString triggerSelector = ui->TriggerSelectBox->currentText();
    int nRet = MV_OK;

    if ("FrameBurstStart" == triggerSelector)
    {
        nRet = m_MyCamera->SetEnumValue("TriggerSelector", FRAMEBURSTSTART);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error", "Set TriggerSelector FrameBurstStart fail!");
            return;
        }

        // 检查是否应启用软触发按钮
        QString triggerMode = ui->TriggerModeBox->currentText();
        QString triggerSource = ui->TriggerSourceBox->currentText();
        m_TriggerModeCheck = (triggerMode == "On" && triggerSource == "Software");
    }
    else if (triggerSelector == "LineStart")
    {
        nRet = m_MyCamera->SetEnumValue("TriggerSelector", LINESTART);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error", "Set TriggerSelector LineStart fail!");
            return;
        }
        m_TriggerModeCheck = false;
    }

    // 刷新触发源显示
    nRet = GetTriggerSource();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Trigger Source Fail!");
    }
}

/**
 * @brief 触发模式切换处理
 * 开启或关闭触发模式，并更新软触发按钮状态
 */
void MainWindow::on_TriggerModeBox_currentIndexChanged()
{
    if (!initialflag)
    {
        return;
    }

    QString triggerMode = ui->TriggerModeBox->currentText();
    int nRet = MV_OK;

    if (triggerMode == "On")
    {
        nRet = m_MyCamera->SetEnumValue("TriggerMode", MV_TRIGGER_MODE_ON);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error", "Set Trigger Mode fail!");
            return;
        }

        // 检查是否应启用软触发按钮
        QString triggerSelector = ui->TriggerSelectBox->currentText();
        QString triggerSource = ui->TriggerSourceBox->currentText();
        m_TriggerModeCheck = (triggerSelector == "FrameBurstStart" && triggerSource == "Software");
    }
    else if (triggerMode == "Off")
    {
        nRet = m_MyCamera->SetEnumValue("TriggerMode", MV_TRIGGER_MODE_OFF);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error", "Set Trigger Mode fail!");
            return;
        }
        m_TriggerModeCheck = false;
    }
}

/**
 * @brief 触发源切换处理
 * 根据下拉框选择设置相应的触发源
 */
void MainWindow::on_TriggerSourceBox_currentIndexChanged()
{
    if (!initialflag)
    {
        return;
    }

    m_TriggerModeCheck = false;
    QString triggerSource = ui->TriggerSourceBox->currentText();

    // 使用 map 查找对应的触发源枚举值
    auto it = m_mapTriggerSource.find(triggerSource);
    if (it != m_mapTriggerSource.end())
    {
        // 设置触发源
        int nRet = m_MyCamera->SetEnumValue("TriggerSource", it->second);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error",
                QString("Set Trigger Source %1 fail!").arg(triggerSource));
            return;
        }

        // 如果是软触发模式，检查是否需要启用软触发按钮
        if (triggerSource == "Software")
        {
            QString triggerSelector = ui->TriggerSelectBox->currentText();
            QString triggerMode = ui->TriggerModeBox->currentText();

            if (triggerSelector == "FrameBurstStart" && triggerMode == "On")
            {
                m_TriggerModeCheck = true;
            }
        }
    }
    else
    {
        QMessageBox::critical(this, "error",
            QString("Unknown trigger source: %1").arg(triggerSource));
    }
}

/**
 * @brief 像素格式切换处理
 * 设置相机像素格式，并在HB模式下刷新图像压缩模式
 */
void MainWindow::on_PixelFormatBox_currentIndexChanged()
{
    if (!initialflag)
    {
        return;
    }

    QString pixelFormat = ui->PixelFormatBox->currentText();

    // 使用 map 查找对应的像素格式枚举值
    auto it = m_mapPixelFormat.find(pixelFormat);
    if (it != m_mapPixelFormat.end())
    {
        int nRet = m_MyCamera->SetEnumValue("PixelFormat", it->second);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error", "Set PixelFormat fail!");
            return;
        }
    }

    // HB模式下需要刷新图像压缩模式
    if (m_HBMode)
    {
        int nRet = GetImageCompressionMode();
        if (nRet != MV_OK)
        {
            QMessageBox::critical(this, "error", "Get Image Compression Mode Fail!");
        }
    }
}

/**
 * @brief 前置增益切换处理
 * 设置相机的前置增益值
 */
void MainWindow::on_PreampGainBox_currentIndexChanged()
{
    if (!initialflag)
    {
        return;
    }

    QString preampGain = ui->PreampGainBox->currentText();

    // 使用 map 查找对应的增益枚举值
    auto it = m_mapPreampGain.find(preampGain);
    if (it != m_mapPreampGain.end())
    {
        int nRet = m_MyCamera->SetEnumValue("PreampGain", it->second);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error", "Set PreampGain fail!");
        }
    }
}

/**
 * @brief HB格式切换处理
 * 开启或关闭图像压缩模式（HB模式）
 */
void MainWindow::on_HBFormatBox_currentIndexChanged()
{
    if (!initialflag)
    {
        return;
    }

    QString compressionMode = ui->HBFormatBox->currentText();
    int nRet = MV_OK;

    if (compressionMode == "Off")
    {
        nRet = m_MyCamera->SetEnumValue("ImageCompressionMode", IMAGE_COMPRESSION_MODE_OFF);
    }
    else if (compressionMode == "HB")
    {
        nRet = m_MyCamera->SetEnumValue("ImageCompressionMode", IMAGE_COMPRESSION_MODE_HB);
    }

    if (MV_OK != nRet)
    {
        QMessageBox::critical(this, "error", "Set Image Compression Mode fail!");
    }
}

/**
 * @brief 行频使能状态切换处理
 * 启用或禁用采集行频控制
 */
void MainWindow::on_AcquisitionLineRateEnableBox_stateChanged()
{
    if (!initialflag)
    {
        return;
    }

    bool enabled = ui->AcquisitionLineRateEnableBox->isChecked();

    // 根据复选框状态控制AcquisitionLineRateEdit的使能状态
    ui->AcquisitionLineRateEdit->setEnabled(enabled && !m_ProcessThread);

    int nRet = m_MyCamera->SetBoolValue("AcquisitionLineRateEnable", enabled);
    if (MV_OK != nRet)
    {
        QMessageBox::critical(this, "error", "Set Acquisition LineRate Enable fail!");
    }
}

/**
 * @brief 选择图像保存路径
 * 弹出目录选择对话框，设置图像保存路径
 */
void MainWindow::on_SelectSavePath_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        "选择保存目录",
        QCoreApplication::applicationDirPath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    if (!dir.isEmpty())
    {
        ui->imgSavePathEdit->setText(QDir::toNativeSeparators(dir));
    }
}

/**
 * @brief 选择模型文件路径
 * 弹出文件选择对话框，选择 TensorRT YOLO 模型文件 (.engine)
 */
void MainWindow::on_SelectModelPath_clicked()
{
    // 获取当前模型路径作为默认打开目录
    QString currentPath = ui->ModelPathEdit->text();
    QString defaultDir;

    if (!currentPath.isEmpty() && QFileInfo(currentPath).exists())
    {
        defaultDir = QFileInfo(currentPath).absolutePath();
    }
    else
    {
        defaultDir = QCoreApplication::applicationDirPath();
    }

    // 打开文件选择对话框
    QString fileName = QFileDialog::getOpenFileName(
        this,
        "选择 YOLO 模型文件",
        defaultDir,
        "TensorRT Engine Files (*.engine);;All Files (*.*)"
    );

    if (fileName.isEmpty())
    {
        return;
    }

    ui->ModelPathEdit->setText(QDir::toNativeSeparators(fileName));

    // 采集期间此按钮已禁用，这里替换模型不会与推理线程冲突
    QString error;
    if (loadModel(fileName, &error))
    {
        // 保持之前的勾选状态
        ui->DetectCheckBox->setEnabled(true);
        QMessageBox::information(this, "模型加载", "YOLO 模型加载成功！\n路径：" + fileName);
    }
    else
    {
        ui->DetectCheckBox->setEnabled(false);
        ui->DetectCheckBox->setChecked(false);
        QMessageBox::critical(this, "模型加载失败", "YOLO 模型加载失败！\n路径：" + fileName + "\n" + error);
    }
    updateModelStatus();
}

/**
 * @brief 把相机当前参数保存到用户集 UserSet1（与"加载参数"对应）
 */
void MainWindow::on_SavePara_clicked()
{
    const int nRet = m_MyCamera->SavePara();
    if (MV_OK != nRet)
    {
        QMessageBox::critical(this, "error", QString("Save parameters fail! (0x%1)").arg(unsigned(nRet), 0, 16));
        return;
    }
    QMessageBox::information(this, "success", "Parameters saved to UserSet1.");
}

/**
 * @brief 加载相机参数
 * 从相机加载已保存的参数配置
 */
void MainWindow::on_LoadPara_clicked()
{
    int nRet = m_MyCamera->LoadPara();
    if (MV_OK != nRet)
    {
        QMessageBox::critical(this, "error", "Load parameters fail!");
    }
}

// ==================== PLC通信功能 ====================

/**
 * @brief 缺陷检测复选框状态改变槽函数
 * @param state 复选框状态（Qt::Unchecked=0, Qt::Checked=2）
 *
 * 根据复选框状态控制缺陷检测功能的启用/禁用：
 * - 选中：启用缺陷检测（前提是YOLO检测器已初始化）
 * - 未选中：禁用缺陷检测
 */
void MainWindow::on_DetectCheckBox_stateChanged(int state)
{
    if (state == Qt::Checked && !m_yoloDetector)
    {
        ui->DetectCheckBox->setChecked(false);
        QMessageBox::warning(this, "缺陷检测",
            "YOLO检测器未初始化！\n"
            "请检查模型文件路径是否正确：\n" + m_modelPath);
    }
}

/**
 * @brief 更新检测耗时显示
 * @param elapsedMs 检测耗时（毫秒）
 *
 * 在DetectTimeLabel上显示检测耗时
 */
void MainWindow::updateDetectionTime(qint64 elapsedMs)
{
    if (ui && ui->DetectTimeLabel)
    {
        ui->DetectTimeLabel->setText(QString("%1 ms").arg(elapsedMs));
    }
}

// ==================== 光源控制功能实现 ====================

/**
 * @brief 更新光源控件使能状态
 * @param isPortOpen true-串口已打开, false-串口未打开
 *
 * 串口打开时启用光源控制按钮，关闭时禁用
 */
void MainWindow::updateLightControlsEnabled(bool isPortOpen)
{
    ui->PortConnect->setEnabled(!isPortOpen);
    ui->PortDisconnect->setEnabled(isPortOpen);
    ui->LightOn->setEnabled(isPortOpen);
    ui->LightOff->setEnabled(isPortOpen);
    ui->LightnessEdit->setEnabled(isPortOpen);
    ui->LightnessSet->setEnabled(isPortOpen);
    ui->LightStatusGet->setEnabled(isPortOpen);
}

/**
 * @brief 串口连接按钮槽函数
 * 根据下拉框选择的串口号打开串口连接
 */
void MainWindow::on_PortConnect_clicked()
{
    QString portName = ui->PortCombobox->currentText();

    // 验证串口选择
    if (portName == "请选择串口" || portName == "未检测到可用串口" || portName.isEmpty())
    {
        QMessageBox::warning(this, "警告", "请先选择有效的串口");
        return;
    }

    // 尝试打开串口
    if (m_lightController->initSerial(portName))
    {
        updateLightControlsEnabled(true);
        setStatusChip(ui->LightStatusLabel, "已连接", "busy");
    }
    else
    {
        QMessageBox::critical(this, "错误", "串口打开失败");
    }
}

/**
 * @brief 串口断开按钮槽函数
 * 关闭当前打开的串口连接
 */
void MainWindow::on_PortDisconnect_clicked()
{
    m_lightController->closeSerial();
    updateLightControlsEnabled(false);
    setStatusChip(ui->LightStatusLabel, "未连接", "off");
}

/**
 * @brief 光源开启按钮槽函数
 * 发送指令将光源设置为常亮状态
 */
void MainWindow::on_LightOn_clicked()
{
    if (m_lightController->setLightOn())
    {
        setStatusChip(ui->LightStatusLabel, "光源: 开启", "ok");
    }
    else
    {
        QMessageBox::warning(this, "错误", "光源开启失败");
    }
}

/**
 * @brief 光源关闭按钮槽函数
 * 发送指令将光源设置为常灭状态
 */
void MainWindow::on_LightOff_clicked()
{
    if (m_lightController->setLightOff())
    {
        setStatusChip(ui->LightStatusLabel, "光源: 关闭", "error");
    }
    else
    {
        QMessageBox::warning(this, "错误", "光源关闭失败");
    }
}

/**
 * @brief 亮度设置按钮槽函数
 * 读取输入框中的亮度值并设置到光源控制器
 */
void MainWindow::on_LightnessSet_clicked()
{
    QString brightnessText = ui->LightnessEdit->text();

    // 检查输入是否为空
    if (brightnessText.isEmpty())
    {
        QMessageBox::warning(this, "警告", "请输入亮度值（0-255）");
        return;
    }

    // 转换为整数并验证范围
    bool ok;
    int brightness = brightnessText.toInt(&ok);
    if (!ok || brightness < 0 || brightness > 255)
    {
        QMessageBox::warning(this, "警告", "亮度值必须在0-255之间");
        return;
    }

    // 设置亮度
    if (!m_lightController->setLightBrightness(brightness))
    {
        QMessageBox::warning(this, "错误", "亮度设置失败");
    }
}

/**
 * @brief 光源状态读取按钮槽函数
 * 查询并显示当前光源的开关状态和亮度值
 */
void MainWindow::on_LightStatusGet_clicked()
{
    // 查询并更新亮度值
    int brightness = m_lightController->queryLightBrightness();
    ui->LightnessEdit->setText(QString::number(brightness));

    // 读取光源开关状态
    bool isOn = m_lightController->isLightOn();
    QString stateText = isOn ? "开启" : "关闭";

    // 更新状态显示
    setStatusChip(ui->LightStatusLabel, QString("光源: %1, 亮度: %2").arg(stateText).arg(brightness), isOn ? "ok" : "error");
}

// ==================== 传送带控制功能实现 ====================

/**
 * @brief 更新传送带控件使能状态
 * @param isPortOpen true-串口已打开, false-串口未打开
 *
 * 串口打开时启用传送带控制按钮，关闭时禁用
 */
void MainWindow::updateConveyorControlsEnabled(bool isPortOpen)
{
    ui->ConConnect->setEnabled(!isPortOpen);
    ui->ConDisconnect->setEnabled(isPortOpen);
    ui->ConForward->setEnabled(isPortOpen);
    ui->ConReverse->setEnabled(isPortOpen);
    ui->ConStop->setEnabled(isPortOpen);
}

/**
 * @brief 更新传送带状态显示
 * @param status 传送带状态字符串
 *
 * 根据状态设置不同的显示颜色
 */
void MainWindow::updateConveyorStatus(const QString &status)
{
    // 根据状态选择显示颜色（颜色定义见 .ui 样式表中的 state 属性）
    const char* state = "busy";
    if (status.contains("未连接"))
        state = "off";
    else if (status.contains("正转"))
        state = "ok";
    else if (status.contains("反转"))
        state = "warn";
    else if (status.contains("停止"))
        state = "error";
    setStatusChip(ui->ConSatus, status, state);
}

/**
 * @brief 传送带串口连接按钮槽函数
 * 根据下拉框选择的串口号打开串口连接
 */
void MainWindow::on_ConConnect_clicked()
{
    QString portName = ui->ConveyorPortComboBox->currentText();

    // 验证串口选择
    if (portName == "请选择串口" || portName == "未检测到可用串口" || portName.isEmpty())
    {
        QMessageBox::warning(this, "警告", "请先选择有效的串口");
        return;
    }

    // 尝试打开串口（使用默认波特率9600）
    if (m_conveyorController->initSerial(portName))
    {
        updateConveyorControlsEnabled(true);
        qDebug() << "传送带串口连接成功:" << portName;
    }
    else
    {
        QMessageBox::critical(this, "错误", "传送带串口打开失败");
    }
}

/**
 * @brief 传送带串口断开按钮槽函数
 * 关闭当前打开的串口连接
 */
void MainWindow::on_ConDisconnect_clicked()
{
    m_conveyorController->closeSerial();
    updateConveyorControlsEnabled(false);
    qDebug() << "传送带串口已断开";
}

/**
 * @brief 传送带正转按钮槽函数
 * 发送正转指令到PLC
 */
void MainWindow::on_ConForward_clicked()
{
    if (!m_conveyorController->setForward())
    {
        QMessageBox::warning(this, "警告", "正转命令发送失败");
    }
}

/**
 * @brief 传送带反转按钮槽函数
 * 发送反转指令到PLC
 */
void MainWindow::on_ConReverse_clicked()
{
    if (!m_conveyorController->setReverse())
    {
        QMessageBox::warning(this, "警告", "反转命令发送失败");
    }
}

/**
 * @brief 传送带停止按钮槽函数
 * 发送停止指令到PLC
 */
void MainWindow::on_ConStop_clicked()
{
    if (!m_conveyorController->setStop())
    {
        QMessageBox::warning(this, "警告", "停止命令发送失败");
    }
}
