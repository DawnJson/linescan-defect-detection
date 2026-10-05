/**
 * @file main.cpp
 * @brief HIKONCam 应用程序入口
 *
 * 海康威视工业相机控制应用程序
 * 功能：相机设备管理、图像采集、图像处理和拼接
 *
 * @author DawnJson
 * @date 2025-10-21
 * @version 2.0
 */

#include "mainwindow.h"
#include "logindialog.h"
#include <QApplication>

/**
 * @brief 应用程序入口函数
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return 应用程序退出码，0表示正常退出
 */
int main(int argc, char *argv[])
{
    // 创建应用程序实例
    QApplication app(argc, argv);

    // 先显示登录对话框
    LoginDialog loginDialog;

    // 如果登录成功（用户点击登录按钮且验证通过）
    if (loginDialog.exec() == QDialog::Accepted) {
        // 创建并显示主窗口
        MainWindow mainWindow;
        mainWindow.show();

        // 进入事件循环
        return app.exec();
    }

    // 登录失败或取消登录，直接退出程序
    return 0;
}
