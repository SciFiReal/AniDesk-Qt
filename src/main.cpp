// ============================================================================
//  main.cpp —— AniDesk-Qt 第一步原型入口
//
//  职责：
//    1. 初始化 QApplication（Qt6 默认已启用高 DPI 缩放）；
//    2. 实例化 PetWidget；
//    3. 将宠物放置到主屏幕可用区域的右下角（自动避开任务栏）；
//    4. 进入事件循环。
// ============================================================================

#include <QApplication>
#include <QGuiApplication>
#include <QMessageBox>
#include <QScreen>
#include <QSharedMemory>

#include "PetWidget.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("AniDesk-Qt"));
    QApplication::setOrganizationName(QStringLiteral("AniDesk"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    // 单实例守卫：重复启动时直接退出，避免桌面上出现多只宠物
    QSharedMemory singleInstanceGuard(QStringLiteral("AniDesk-Qt.SingleInstance"));
    if (!singleInstanceGuard.create(1)) {
        QMessageBox::information(nullptr,
                                 QStringLiteral("AniDesk"),
                                 QStringLiteral("桌宠已经在运行啦～（就在屏幕右下角）"));
        return 0;
    }

    PetWidget pet;

    // 放置到主屏幕可用区域的右下角。
    // availableGeometry() 已扣除任务栏；rect 的 right()/bottom() 为闭区间端点，故 +1 修正。
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    const int margin = 36;
    pet.move(avail.right() - pet.width() + 1 - margin,
             avail.bottom() - pet.height() + 1 - margin);

    pet.show();

    return QApplication::exec();
}
