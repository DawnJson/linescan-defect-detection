#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <map>
#include <memory>
#include "MvCamera.h"
#include "processthread.h"
#include "arrayqueue.h"
#include "trtyolo.hpp"
#include "HikLightController.h"
#include "ConveyorController.h"

class QComboBox;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

//触发选项
typedef enum _MV_CAM_TRIGGER_OPTION_
{
    FRAMEBURSTSTART = 6,
    LINESTART = 9,
}MV_CAM_TRIGGER_OPTION;

//ImageCompressionMode模式
typedef enum _MV_IMAGE_COMPRESSION_MODE_
{
    IMAGE_COMPRESSION_MODE_OFF = 0,
    IMAGE_COMPRESSION_MODE_HB = 2,
}MV_IMAGE_COMPRESSION_MODE;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 触发选项
    int GetTriggerSelector();

    // 触发模式
    int GetTriggerMode();

    // 曝光时间
    int GetExposureTime();
    int SetExposureTime();

    // 增益
    int GetPreampGain();
    int GetDigitalGain();
    int SetDigitalGain();

    // 行频
    int GetAcquisitionLineRateEnable();
    int GetAcquisitionLineRate();
    int GetResultingLineRate();
    int SetAcquisitionLineRate();
    int GetResultingFrameRate();

    // HB模式
    int GetImageCompressionMode();

    // 触发源
    int GetTriggerSource();

    // PixelFormat
    int GetPixelFormat();

    // 采集宽度、高度
    int GetWidthHeight();
    int SetWidthHeight();

    int GetAcquisitionBurstFrameCount();
    int SetAcquisitionBurstFrameCount();

public slots:

    // 查找设备
    void on_SearchDevice_clicked();

    // 打开设备
    void on_DeviceOpen_clicked();

    // 关闭设备
    void on_DeviceClose_clicked();

    // 获取相机参数
    void on_GetPara_clicked();

    // 设置相机参数
    void on_SetPara_clicked();

    // 开始采集
    void on_StartGrab_clicked();

    // 结束采集
    void on_StopGrab_clicked();

    // 软触发一次
    void on_SingleSoftTrigger_clicked();

    void on_TriggerSelectBox_currentIndexChanged();
    void on_TriggerModeBox_currentIndexChanged();
    void on_TriggerSourceBox_currentIndexChanged();
    void on_PixelFormatBox_currentIndexChanged();
    void on_PreampGainBox_currentIndexChanged();
    void on_HBFormatBox_currentIndexChanged();

    void on_AcquisitionLineRateEnableBox_stateChanged();

    void on_SelectSavePath_clicked();
    void on_SelectModelPath_clicked();

    void on_SavePara_clicked();
    void on_LoadPara_clicked();

    // 缺陷检测槽函数
    void on_DetectCheckBox_stateChanged(int state);

    // 更新检测耗时槽函数
    void updateDetectionTime(qint64 elapsedMs);

    // 光源控制槽函数
    void on_PortConnect_clicked();
    void on_PortDisconnect_clicked();
    void on_LightOn_clicked();
    void on_LightOff_clicked();
    void on_LightnessSet_clicked();
    void on_LightStatusGet_clicked();

    // 传送带控制槽函数
    void on_ConConnect_clicked();
    void on_ConDisconnect_clicked();
    void on_ConForward_clicked();
    void on_ConReverse_clicked();
    void on_ConStop_clicked();

private:
    Ui::MainWindow *ui;
    bool initialflag;

    MV_CC_DEVICE_INFO_LIST  m_stDevList; // 设备信息

    std::unique_ptr<CMvCamera> m_MyCamera;          // 相机，打开设备时创建
    std::unique_ptr<ArrayQueue> m_queue;            // 回调 → 处理线程的帧队列，采集期间存在
    std::unique_ptr<ProcessThread> m_ProcessThread; // 图像处理线程，采集期间存在

    // 配置映射表
    std::map<QString, int> m_mapTriggerSource;  // 触发源映射
    std::map<QString, int> m_mapPreampGain;     // 前置增益映射
    std::map<QString, int> m_mapPixelFormat;    // 像素格式映射

    // 控制标志位
    bool m_TriggerModeCheck;       // 软触发使能标志
    bool m_bAcquisitionLineRate;   // 是否启用行频控制
    bool m_HBMode;                 // 是否支持HB压缩模式

    static void __stdcall ImageCallBack(unsigned char * pData, MV_FRAME_OUT_INFO_EX* pFrameInfo, void* pUser);

    // 读取相机枚举参数的全部选项填入下拉框并选中当前值；map 非空时记录 名称→枚举值
    int fillEnumCombo(const char* key, QComboBox* box, std::map<QString, int>* map = nullptr);

    // 停止采集：停相机、注销回调、停处理线程、释放队列。未在采集时无操作
    void stopGrabbing();

    // 从界面读取处理线程所需的参数快照
    ProcessConfig makeProcessConfig() const;

    // 加载检测模型，失败时返回 false 并写入 error
    bool loadModel(const QString& path, QString* error);

    // UI控制辅助函数
    void setDeviceControlsEnabled(bool enable);       // 设备控制按钮
    void setParameterControlsEnabled(bool enable);    // 参数控制控件
    void setAcquisitionControlsEnabled(bool enable);  // 采集控制控件
    void setGrabbingControlsEnabled(bool isGrabbing); // 采集状态控件

    // 底部状态条显示：state 取 ok / busy / warn / off / error，对应样式表中的颜色
    void setStatusChip(QLabel* label, const QString& text, const char* state);
    void updateModelStatus();                         // 按模型是否加载刷新"检测模型"状态

    // YOLO检测器相关成员变量
    std::unique_ptr<trtyolo::DetectModel> m_yoloDetector;  // YOLO检测模型
    QString m_modelPath;                                   // 模型文件路径

    // ==================== 光源控制器 ====================
    std::unique_ptr<HikLightController> m_lightController;  // 光源控制器对象

    // 光源控制辅助函数
    void updateLightControlsEnabled(bool isPortOpen);  // 更新控件使能状态

    // ==================== 传送带控制器 ====================
    ConveyorController* m_conveyorController;   // 传送带控制器对象（子对象，随窗口释放）

    // 传送带控制辅助函数
    void updateConveyorControlsEnabled(bool isPortOpen);  // 更新传送带控件使能状态
    void updateConveyorStatus(const QString &status);     // 更新传送带状态显示

    static void fillSerialPorts(QComboBox* box);          // 把可用串口填入下拉框
};
#endif // MAINWINDOW_H
