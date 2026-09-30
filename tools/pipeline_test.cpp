// ============================================================================
//  pipeline_test —— AI 资产管线自检工具（不依赖拖拽，直接驱动 AssetPipelineManager）
//
//  用途：验证"拖入静态图 → 抠图(未部署则降级) → 动图/人设(未部署则降级) →
//        打包 PetAssetPackage"的完整降级链路。
//
//  构建（EXCLUDE_FROM_ALL，不影响主程序）：
//    cmake --build build --target anidesk-pipeline-test
//  运行：
//    build/anidesk-pipeline-test.exe
// ============================================================================
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QTimer>

#include <cstdio>

#include "AssetPipelineManager.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);   // 控制台按 UTF-8 输出中文
#endif

    // 1) 在 exe 目录生成一张测试图（透明底 + 圆形角色）
    const QString imgPath = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/pipeline_test_input.png");
    QImage image(256, 256, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(111, 199, 255));
    p.setPen(QPen(QColor(52, 120, 180), 4));
    p.drawEllipse(32, 32, 192, 192);
    p.setBrush(Qt::white);
    p.drawEllipse(88, 96, 24, 28);
    p.drawEllipse(144, 96, 24, 28);
    p.end();
    image.save(imgPath, "PNG");
    printf("[setup] test image: %s\n", qPrintable(imgPath));

    // 2) 驱动管线（与主程序 setupAiServices 相同的配置）
    AssetPipelineManager pipeline;
    pipeline.setVisionApi(QStringLiteral("http://localhost:11434/v1"),
                          QString(),
                          QStringLiteral("qwen2.5vl:3b"));

    QObject::connect(&pipeline, &AssetPipelineManager::pipelineProgress,
                     [](const QString &step, int percent) {
                         printf("[progress %3d%%] %s\n", percent, step.toUtf8().constData());
                         fflush(stdout);
                     });
    QObject::connect(&pipeline, &AssetPipelineManager::pipelineFailed,
                     [](const QString &error) {
                         printf("[FAILED] %s\n", error.toUtf8().constData());
                         QCoreApplication::quit();
                     });
    QObject::connect(&pipeline, &AssetPipelineManager::pipelineCompleted,
                     [](const PetAssetPackage &pkg) {
                         printf("[COMPLETED] PetAssetPackage:\n");
                         printf("  name           = %s\n", pkg.name.toUtf8().constData());
                         printf("  greeting       = %s\n", pkg.greeting.toUtf8().constData());
                         printf("  systemPrompt   = %s\n",
                                QString(pkg.systemPrompt).replace(QLatin1Char('\n'), " / ")
                                    .toUtf8().constData());
                         printf("  idleGifPath    = %s\n", pkg.idleGifPath.toUtf8().constData());
                         printf("  talkingGifPath = %s\n", pkg.talkingGifPath.toUtf8().constData());
                         printf("  happyGifPath   = %s\n", pkg.happyGifPath.toUtf8().constData());
                         QCoreApplication::quit();
                     });

    pipeline.processImage(imgPath);

    // 60s 兜底超时（正常应在数秒内完成）
    QTimer::singleShot(300000, &app, [] {
        printf("[TIMEOUT] pipeline did not finish in 60s\n");
        QCoreApplication::quit();
    });

    return QCoreApplication::exec();
}
