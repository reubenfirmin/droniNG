/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <coreplugin/iboardtype.h>
#include <coreplugin/firmwarecatalog.h>

/* A shared native profile interface, independent of legacy Brain/RE1 IDs. */
class ImportedBoard final : public Core::IBoardType
{
public:
    explicit ImportedBoard(const Core::FirmwareBoardProfile &profile);
    QString shortName() override { return m_profile.name; }
    QString boardDescription() override { return m_profile.notes; }
    QString getHwUAVO() override { return "HwImported"; }
    QPixmap getBoardPicture() override { return QPixmap(":/core/images/cpu.png"); }
    bool queryCapabilities(BoardCapabilities capability) override;
    bool isInputConfigurationSupported(InputType type = INPUT_TYPE_ANY) override;
    bool setInputType(InputType type) override;
    InputType getInputType() override;
    int queryMaxGyroRate() override { return m_profile.id == "radix" ? 1600 : 1000; }
    QVector<QVector<int>> getChannelBanks() override {
        return m_profile.id == "radix" ? QVector<QVector<int>>{{1, 2}, {3}, {4}}
                                      : QVector<QVector<int>>{{1, 2}, {3, 4}};
    }
    bool supportsDShot() const override { return false; }
    QStringList getAdcNames() override { return {"I", "V", "R"}; }
    bool hasAnnunciator(AnnunciatorType type) override;
    QWidget *getBoardConfiguration(QWidget *parent, bool connected) override;
private:
    Core::FirmwareBoardProfile m_profile;
};
