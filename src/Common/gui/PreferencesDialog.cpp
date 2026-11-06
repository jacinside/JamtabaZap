#include "PreferencesDialog.h"
#include "ui_PreferencesDialog.h"

#include <QtWidgets>
#include <QDebug>
#include <QFileDialog>
#include <QDateTime>
#include "persistence/Settings.h"
#include "MetronomeUtils.h"
#include "video/VideoPresets.h"
#include "audio/vorbis/Vorbis.h"

PreferencesDialog::PreferencesDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::PreferencesDialog)
{
    ui->setupUi(this);
    setModal(true);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint & Qt::WA_DeleteOnClose);

    ui->comboLastOutput->setEnabled(false);

    connect(ui->recordPathLineEdit, &QLineEdit::textEdited, this, &PreferencesDialog::recordingPathSelected);

    // populate bit rate combo box
    quint8 bitRates[] = {16, 32};
    ui->comboBoxBitRate->clear();
    for (quint8 bitRate : bitRates) {
        ui->comboBoxBitRate->addItem(QString::number(bitRate) + " bits", QVariant::fromValue(bitRate));
    }
    connect(ui->comboBoxBitRate, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), [=](){
        quint8 bitRate = ui->comboBoxBitRate->currentData().toInt();
        emit looperWaveFilesBitDepthChanged(bitRate);
    });

    connect(ui->radioButtonWaveFile, &QRadioButton::toggled, ui->comboBoxBitRate, &QComboBox::setEnabled);
    connect(ui->radioButtonWaveFile, &QRadioButton::toggled, ui->labelBitRate, &QComboBox::setEnabled);
}

void PreferencesDialog::toggleBuiltInMetronomeSounds(bool usingBuiltInMetronome)
{
    ui->groupBoxCustomMetronome->setChecked(!usingBuiltInMetronome);
    refreshMetronomeControlsStyleSheet();
}

void PreferencesDialog::toggleCustomMetronomeSounds(bool usingCustomMetronome)
{
    ui->groupBoxBuiltInMetronomes->setChecked(!usingCustomMetronome);
    refreshMetronomeControlsStyleSheet();
}

void PreferencesDialog::refreshMetronomeControlsStyleSheet()
{
    style()->unpolish(ui->groupBoxCustomMetronome);
    style()->unpolish(ui->groupBoxBuiltInMetronomes);
    style()->unpolish(ui->textFieldPrimaryBeat);
    style()->unpolish(ui->textFieldOffBeat);
    style()->unpolish(ui->textFieldAccentBeat);
    style()->unpolish(ui->browsePrimaryBeatButton);
    style()->unpolish(ui->browseOffBeatButton);
    style()->unpolish(ui->browseAccentBeatButton);

    style()->polish(ui->groupBoxCustomMetronome);
    style()->polish(ui->groupBoxBuiltInMetronomes);
    style()->polish(ui->textFieldPrimaryBeat);
    style()->polish(ui->textFieldOffBeat);
    style()->polish(ui->textFieldAccentBeat);
    style()->polish(ui->browsePrimaryBeatButton);
    style()->polish(ui->browseOffBeatButton);
    style()->polish(ui->browseAccentBeatButton);
}

void PreferencesDialog::initialize(PreferencesTab initialTab, const persistence::Settings *settings, const QMap<QString, QString> &jamRecorders)
{
    Q_UNUSED(initialTab);
    this->settings = settings;
    this->jamRecorders = jamRecorders;
    createVideoTab();
    this->jamRecorderCheckBoxes = QMap<QCheckBox *, QString>();
    this->jamDateFormatRadioButtons = QMap<const QRadioButton *, QString>();

    for (const auto &jamRecorder : jamRecorders.keys()) {
        QCheckBox *myCheckBox = new QCheckBox(this);
        myCheckBox->setObjectName(jamRecorder);
        myCheckBox->setText(jamRecorders.value(jamRecorder));
        ui->layoutRecorders->addWidget(myCheckBox);
        jamRecorderCheckBoxes[myCheckBox] = jamRecorder;
    }

    QDateTime now = QDateTime::currentDateTime();
    Qt::DateFormat dateFormat;
    QString nowString;
    QRadioButton *myRadioButton;

    dateFormat = Qt::TextDate;
    nowString = "Jam-" + now.toString(dateFormat).replace(QRegExp("[/:]"), "-").replace(QRegExp("[ ]"), "_");
    myRadioButton = new QRadioButton(this);
    myRadioButton->setObjectName("rbdfTextDate");
    myRadioButton->setText(nowString);
    myRadioButton->setProperty("buttonGroup", "rbDateFormat");
    ui->layoutDateFormats->addWidget(myRadioButton);
    jamDateFormatRadioButtons[myRadioButton] = "Qt::TextDate";

    dateFormat = Qt::ISODate;
    nowString = "Jam-" + now.toString(dateFormat).replace(QRegExp("[/:]"), "-").replace(QRegExp("[ ]"), "_");
    myRadioButton = new QRadioButton(this);
    myRadioButton->setObjectName("rbdfISODate");
    myRadioButton->setText(nowString);
    myRadioButton->setProperty("buttonGroup", "rbDateFormat");
    ui->layoutDateFormats->addWidget(myRadioButton);
    jamDateFormatRadioButtons[myRadioButton] = "Qt::ISODate";

    setupSignals();

    populateAllTabs();
}

void PreferencesDialog::setupSignals()
{
    // the 'accept' slot is overrided in inherited classes (StandalonePreferencesDialog and VstPreferencesDialog)
    connect(ui->okButton, SIGNAL(clicked(bool)), this, SLOT(accept()));
    connect(ui->prefsTab, SIGNAL(currentChanged(int)), this, SLOT(selectTab(int)));

    connect(ui->recordingCheckBox, SIGNAL(clicked(bool)), this, SLOT(toggleRecording(bool)));
    connect(ui->browseRecPathButton, SIGNAL(clicked(bool)), this, SLOT(openRecordingPathBrowser()));
    for(const QRadioButton *rb : jamDateFormatRadioButtons.keys()) {
        connect(rb, &QRadioButton::toggled, [=]() {
            if (rb->isChecked()) {
                emit jamDateFormatChanged(jamDateFormatRadioButtons[rb]);
            }
        });
    }

    connect(ui->groupBoxCustomMetronome, SIGNAL(toggled(bool)), this, SLOT(toggleCustomMetronomeSounds(bool)));
    connect(ui->groupBoxBuiltInMetronomes, SIGNAL(toggled(bool)), this, SLOT(toggleBuiltInMetronomeSounds(bool)));
    connect(ui->browsePrimaryBeatButton, SIGNAL(clicked(bool)), this, SLOT(openPrimaryBeatAudioFileBrowser()));
    connect(ui->browseOffBeatButton, SIGNAL(clicked(bool)), this, SLOT(openOffBeatAudioFileBrowser()));
    connect(ui->browseAccentBeatButton, SIGNAL(clicked(bool)), this, SLOT(openAccentBeatAudioFileBrowser()));

    connect(ui->comboBoxEncoderQuality, SIGNAL(activated(int)), this, SLOT(emitEncodingQualityChanged()));

    connect(ui->radioButtonLooperOggEncoding, &QCheckBox::toggled, this, &PreferencesDialog::looperAudioEncodingFlagChanged);
    connect(ui->lineEditLoopsFolder, &QLineEdit::textChanged, this,  &PreferencesDialog::looperFolderChanged);
    connect(ui->loopsFolderBrowseButton, &QPushButton::clicked, [=]() {
        QString currentLoopsFolder = ui->lineEditLoopsFolder->text();
        QFileDialog folderDialog(this, tr("Choosing loops folder ..."), currentLoopsFolder);
        folderDialog.setAcceptMode(QFileDialog::AcceptOpen);
        folderDialog.setFileMode(QFileDialog::DirectoryOnly);
        if (folderDialog.exec()) {
            QDir dir = folderDialog.directory();
            QString newLoopsFolder = dir.absolutePath();
            ui->lineEditLoopsFolder->setText(newLoopsFolder);
        }
    });
}

void PreferencesDialog::toggleRecording(bool recording)
{
    for (QCheckBox *checkbox : jamRecorderCheckBoxes.keys())
        checkbox->setChecked(recording);

    emit multiTrackRecordingStatusChanged(recording);
}

void PreferencesDialog::emitEncodingQualityChanged()
{
    QVariant currentData = ui->comboBoxEncoderQuality->currentData();
    if (!currentData.isNull()) {
        emit audioBitrateChanged(currentData.toInt());
    }
}

void PreferencesDialog::accept()
{
    if (ui->groupBoxBuiltInMetronomes->isChecked()) {
        emit builtInMetronomeSelected(ui->comboBuiltInMetronomes->currentText());
    }
    else {
        emit customMetronomeSelected(ui->textFieldPrimaryBeat->text(), ui->textFieldOffBeat->text(), ui->textFieldAccentBeat->text());
    }

    for (auto checkBox : jamRecorderCheckBoxes.keys()) {
        QString jamMetaDataWriterID = jamRecorderCheckBoxes[checkBox];
        emit jamRecorderStatusChanged(jamMetaDataWriterID, checkBox->isChecked());
    }

    bool rememberingBoost = ui->checkBoxRememberBoost->isChecked();
    bool rememberingLevel = ui->checkBoxRememberLevel->isChecked();
    bool rememberingPan = ui->checkBoxRememberPan->isChecked();
    bool rememberingMute = ui->checkBoxRememberMute->isChecked();
    bool rememberingLowCut = ui->checkBoxRememberLowCut->isChecked();
    emit rememberRemoteUserSettingsChanged(rememberingBoost, rememberingLevel, rememberingPan, rememberingMute, rememberingLowCut);

    bool rememberLocalChannels = ui->checkBoxRememberLocalChannels->isChecked();
    bool rememberBottomSection = ui->checkBoxRememberBottomSection->isChecked();
    bool rememberChatSection = ui->checkBoxRememberChatSection->isChecked();
    emit rememberCollapsibleSectionsSettingsChanged(rememberLocalChannels, rememberBottomSection, rememberChatSection);

    QDialog::accept();
}

void PreferencesDialog::createVideoTab()
{
    auto tab = new QWidget(this);
    auto layout = new QFormLayout(tab);

    auto quality = new QComboBox(tab);
    quality->setObjectName(QStringLiteral("comboBoxVideoQuality"));
    for (int i = 0; i < VideoPresets::Count; ++i) {
        const auto &preset = VideoPresets::get(i);
        quality->addItem(QString("%1 (%2x%3, %4 fps, %5 kbps)").arg(QString::fromLatin1(preset.name))
                         .arg(preset.size.width()).arg(preset.size.height()).arg(preset.fps).arg(preset.bitrate / 1000));
    }
    quality->setCurrentIndex(settings->getVideoPreset());
    connect(quality, QOverload<int>::of(&QComboBox::activated), this, &PreferencesDialog::videoPresetChanged);
    layout->addRow(tr("Video quality you send:"), quality);

    auto note = new QLabel(tr("On servers without video support, video stays in SD."), tab);
    note->setWordWrap(true);
    layout->addRow(QString(), note);

    auto receive = new QCheckBox(tr("Receive video from other musicians"), tab);
    receive->setChecked(settings->isReceivingVideo());
    connect(receive, &QCheckBox::toggled, this, &PreferencesDialog::receiveVideoChanged);
    layout->addRow(QString(), receive);

    ui->prefsTab->addTab(tab, tr("Video"));
}

void PreferencesDialog::populateEncoderQualityComboBox()
{
    ui->comboBoxEncoderQuality->clear();
    ui->comboBoxEncoderQuality->addItem(tr("Economy (64 kbps)"), 64);
    ui->comboBoxEncoderQuality->addItem(tr("Standard (128 kbps, default)"), 128);
    ui->comboBoxEncoderQuality->addItem(tr("High (192 kbps)"), 192);
    ui->comboBoxEncoderQuality->addItem(tr("Studio (256 kbps)"), 256);

    const int index = ui->comboBoxEncoderQuality->findData(settings->getAudioBitrate());
    ui->comboBoxEncoderQuality->setCurrentIndex(index >= 0 ? index : 1);
}

bool PreferencesDialog::usingCustomEncodingQuality()
{
    float currentQuality = settings->getEncodingQuality();

    if (qFuzzyCompare(currentQuality, vorbis::EncoderQualityLow))
        return false;

    if (qFuzzyCompare(currentQuality, vorbis::EncoderQualityNormal))
        return false;

    if (qFuzzyCompare(currentQuality, vorbis::EncoderQualityHigh))
        return false;

    return true;
}

void PreferencesDialog::populateAllTabs()
{
    populateEncoderQualityComboBox();
    populateMultiTrackRecordingTab();
    populateMetronomeTab();
    populateLooperTab();
    populateRememberTab();
}

void PreferencesDialog::populateRememberTab()
{
    ui->checkBoxRememberBoost->setChecked(settings->isRememberingBoost());
    ui->checkBoxRememberLevel->setChecked(settings->isRememberingLevel());
    ui->checkBoxRememberPan->setChecked(settings->isRememberingPan());
    ui->checkBoxRememberMute->setChecked(settings->isRememberingMute());
    ui->checkBoxRememberLowCut->setChecked(settings->isRememberingLowCut());

    ui->checkBoxRememberLocalChannels->setChecked(settings->isRememberingLocalChannels());
    ui->checkBoxRememberBottomSection->setChecked(settings->isRememberingBottomSection());
    ui->checkBoxRememberChatSection->setChecked(settings->isRememberingChatSection());
}

void PreferencesDialog::populateLooperTab()
{
    QSignalBlocker lineEditSignalBlocker(ui->lineEditLoopsFolder);
    QSignalBlocker radioButtonSignalBlocker(ui->radioButtonLooperOggEncoding);
    QSignalBlocker bitDepthCheckBoxBlocker(ui->comboBoxBitRate);

    ui->lineEditLoopsFolder->setText(settings->getLooperFolder());
    ui->radioButtonLooperOggEncoding->setChecked(settings->getLooperAudioEncodingFlag());
    ui->radioButtonWaveFile->setChecked(!ui->radioButtonLooperOggEncoding->isChecked());

    ui->comboBoxBitRate->setEnabled(!settings->getLooperAudioEncodingFlag());
    ui->labelBitRate->setEnabled(ui->comboBoxBitRate->isEnabled());

    quint8 bitDepth = settings->getLooperBitDepth();
    quint8 comboBoxIndex = 0; // 16 bits
    if (bitDepth == 32)
        comboBoxIndex = 1;

    ui->comboBoxBitRate->setCurrentIndex(comboBoxIndex);
}

void PreferencesDialog::selectRecordingTab()
{
    ui->prefsTab->setCurrentWidget(ui->tabRecording);
}

void PreferencesDialog::populateMetronomeTab()
{
    Q_ASSERT(settings);

    bool usingCustomMetronomeSounds = settings->isUsingCustomMetronomeSounds();
    ui->groupBoxCustomMetronome->setChecked(usingCustomMetronomeSounds);
    ui->groupBoxBuiltInMetronomes->setChecked(!usingCustomMetronomeSounds);
    ui->textFieldPrimaryBeat->setText(settings->getMetronomeFirstBeatFile());
    ui->textFieldOffBeat->setText(settings->getMetronomeOffBeatFile());

    // combo embedded metronome sounds
    auto metronomeAliases = audio::metronomeUtils::getBuiltInMetronomeAliases();
    ui->comboBuiltInMetronomes->clear();
    for (QString alias : metronomeAliases) {
        ui->comboBuiltInMetronomes->addItem(alias, alias);
    }

    // using built-in metronome?
    if (!settings->isUsingCustomMetronomeSounds()){
        ui->comboBuiltInMetronomes->setCurrentText(settings->getBuiltInMetronome());
    }
}

void PreferencesDialog::populateMultiTrackRecordingTab()
{
    Q_ASSERT(settings);
    auto recordingSettings = settings->getMultiTrackRecordingSettings();
    ui->recordingCheckBox->setChecked(recordingSettings.saveMultiTracksActivated);

    for (auto myCheckBox : jamRecorderCheckBoxes.keys()) {
        myCheckBox->setChecked(recordingSettings.isJamRecorderActivated(jamRecorderCheckBoxes[myCheckBox]));
    }

    for (const QRadioButton * myRadioButton : jamDateFormatRadioButtons.keys()) {
        ((QRadioButton *)myRadioButton)->setChecked(QString::compare(recordingSettings.dirNameDateFormat, jamDateFormatRadioButtons[myRadioButton]) == 0);
    }

    QDir recordDir(recordingSettings.recordingPath);
    ui->recordPathLineEdit->setText(recordDir.absolutePath());
}

PreferencesDialog::~PreferencesDialog()
{
    delete ui;
}

QString PreferencesDialog::getAudioFilesFilter()
{
    return tr("Audio Files") + " (*.wav *.ogg)";
}

void PreferencesDialog::openPrimaryBeatAudioFileBrowser()
{
    QString caption = tr("Choosing Primary beat audio file...");
    QString filter = getAudioFilesFilter();
    QString dir = ".";
    QString filePath = QFileDialog::getOpenFileName(this, caption, dir, filter);
    if (!filePath.isNull()) {
        ui->textFieldPrimaryBeat->setText(filePath);
    }
}

void PreferencesDialog::openOffBeatAudioFileBrowser()
{
    QString caption = tr("Choosing Off beat audio file...");
    QString filter = getAudioFilesFilter();
    QString dir = ".";
    QString filePath = QFileDialog::getOpenFileName(this, caption, dir, filter);
    if (!filePath.isNull()) {
        ui->textFieldOffBeat->setText(filePath);
    }
}

void PreferencesDialog::openAccentBeatAudioFileBrowser()
{
    QString caption = tr("Choosing Accent beat audio file...");
    QString filter = getAudioFilesFilter();
    QString dir = ".";
    QString filePath = QFileDialog::getOpenFileName(this, caption, dir, filter);
    if (!filePath.isNull()) {
        ui->textFieldAccentBeat->setText(filePath);
    }
}

QString PreferencesDialog::openAudioFileBrowser(const QString caption)
{
    QString filter = tr("Audio Files") + " (*.wav, *.ogg)";
    QString dir = ".";

    return QFileDialog::getOpenFileName(this, caption, dir, filter);
}

void PreferencesDialog::openRecordingPathBrowser()
{
    QFileDialog fileDialog(this, tr("Choosing recording path ..."));
    fileDialog.setAcceptMode(QFileDialog::AcceptOpen);
    fileDialog.setFileMode(QFileDialog::DirectoryOnly);

    if (fileDialog.exec()) {
        QDir dir = fileDialog.directory();
        QString newRecordingPath = dir.absolutePath();
        ui->recordPathLineEdit->setText(newRecordingPath);
        emit recordingPathSelected(newRecordingPath);
    }
}
