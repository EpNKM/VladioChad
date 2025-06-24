/********************************************************************************
** Form generated from reading UI file 'chatwindow.ui'
**
** Created by: Qt User Interface Compiler version 6.9.1
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_CHATWINDOW_H
#define UI_CHATWINDOW_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_ChatWindow
{
public:
    QWidget *centralwidget;
    QVBoxLayout *verticalLayout;
    QHBoxLayout *horizontalLayout;
    QGroupBox *groupBox;
    QVBoxLayout *verticalLayout_3;
    QLabel *localVideoLabel;
    QGroupBox *groupBox_2;
    QVBoxLayout *verticalLayout_4;
    QLabel *remoteVideoLabel;
    QHBoxLayout *horizontalLayout_2;
    QTextEdit *chatArea;
    QHBoxLayout *horizontalLayout_3;
    QLineEdit *messageEdit;
    QPushButton *sendButton;
    QGroupBox *groupBox_3;
    QVBoxLayout *verticalLayout_2;
    QTextEdit *debugArea;

    void setupUi(QMainWindow *ChatWindow)
    {
        if (ChatWindow->objectName().isEmpty())
            ChatWindow->setObjectName("ChatWindow");
        ChatWindow->resize(1000, 700);
        centralwidget = new QWidget(ChatWindow);
        centralwidget->setObjectName("centralwidget");
        verticalLayout = new QVBoxLayout(centralwidget);
        verticalLayout->setObjectName("verticalLayout");
        horizontalLayout = new QHBoxLayout();
        horizontalLayout->setObjectName("horizontalLayout");
        groupBox = new QGroupBox(centralwidget);
        groupBox->setObjectName("groupBox");
        verticalLayout_3 = new QVBoxLayout(groupBox);
        verticalLayout_3->setObjectName("verticalLayout_3");
        localVideoLabel = new QLabel(groupBox);
        localVideoLabel->setObjectName("localVideoLabel");
        localVideoLabel->setMinimumSize(QSize(320, 240));
        localVideoLabel->setAlignment(Qt::AlignCenter);

        verticalLayout_3->addWidget(localVideoLabel);


        horizontalLayout->addWidget(groupBox);

        groupBox_2 = new QGroupBox(centralwidget);
        groupBox_2->setObjectName("groupBox_2");
        verticalLayout_4 = new QVBoxLayout(groupBox_2);
        verticalLayout_4->setObjectName("verticalLayout_4");
        remoteVideoLabel = new QLabel(groupBox_2);
        remoteVideoLabel->setObjectName("remoteVideoLabel");
        remoteVideoLabel->setMinimumSize(QSize(320, 240));
        remoteVideoLabel->setAlignment(Qt::AlignCenter);

        verticalLayout_4->addWidget(remoteVideoLabel);


        horizontalLayout->addWidget(groupBox_2);


        verticalLayout->addLayout(horizontalLayout);

        horizontalLayout_2 = new QHBoxLayout();
        horizontalLayout_2->setObjectName("horizontalLayout_2");
        chatArea = new QTextEdit(centralwidget);
        chatArea->setObjectName("chatArea");
        chatArea->setMinimumSize(QSize(0, 150));

        horizontalLayout_2->addWidget(chatArea);


        verticalLayout->addLayout(horizontalLayout_2);

        horizontalLayout_3 = new QHBoxLayout();
        horizontalLayout_3->setObjectName("horizontalLayout_3");
        messageEdit = new QLineEdit(centralwidget);
        messageEdit->setObjectName("messageEdit");

        horizontalLayout_3->addWidget(messageEdit);

        sendButton = new QPushButton(centralwidget);
        sendButton->setObjectName("sendButton");

        horizontalLayout_3->addWidget(sendButton);


        verticalLayout->addLayout(horizontalLayout_3);

        groupBox_3 = new QGroupBox(centralwidget);
        groupBox_3->setObjectName("groupBox_3");
        verticalLayout_2 = new QVBoxLayout(groupBox_3);
        verticalLayout_2->setObjectName("verticalLayout_2");
        debugArea = new QTextEdit(groupBox_3);
        debugArea->setObjectName("debugArea");
        debugArea->setMinimumSize(QSize(0, 100));
        debugArea->setReadOnly(true);

        verticalLayout_2->addWidget(debugArea);


        verticalLayout->addWidget(groupBox_3);

        ChatWindow->setCentralWidget(centralwidget);

        retranslateUi(ChatWindow);

        QMetaObject::connectSlotsByName(ChatWindow);
    } // setupUi

    void retranslateUi(QMainWindow *ChatWindow)
    {
        ChatWindow->setWindowTitle(QCoreApplication::translate("ChatWindow", "\320\233\320\276\320\272\320\260\320\273\321\214\320\275\321\213\320\271 \320\262\320\270\320\264\320\265\320\276\321\207\320\260\321\202", nullptr));
        groupBox->setTitle(QCoreApplication::translate("ChatWindow", "\320\233\320\276\320\272\320\260\320\273\321\214\320\275\320\276\320\265 \320\262\320\270\320\264\320\265\320\276", nullptr));
        localVideoLabel->setText(QCoreApplication::translate("ChatWindow", "\320\232\320\260\320\274\320\265\321\200\320\260 \320\275\320\265 \320\264\320\276\321\201\321\202\321\203\320\277\320\275\320\260", nullptr));
        groupBox_2->setTitle(QCoreApplication::translate("ChatWindow", "\320\243\320\264\320\260\320\273\320\265\320\275\320\275\320\276\320\265 \320\262\320\270\320\264\320\265\320\276", nullptr));
        remoteVideoLabel->setText(QCoreApplication::translate("ChatWindow", "\320\236\320\266\320\270\320\264\320\260\320\275\320\270\320\265 \320\277\320\276\320\264\320\272\320\273\321\216\321\207\320\265\320\275\320\270\321\217...", nullptr));
        sendButton->setText(QCoreApplication::translate("ChatWindow", "\320\236\321\202\320\277\321\200\320\260\320\262\320\270\321\202\321\214", nullptr));
        groupBox_3->setTitle(QCoreApplication::translate("ChatWindow", "\320\236\321\202\320\273\320\260\320\264\320\272\320\260", nullptr));
    } // retranslateUi

};

namespace Ui {
    class ChatWindow: public Ui_ChatWindow {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_CHATWINDOW_H
