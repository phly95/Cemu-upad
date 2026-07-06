#include "gui/wxgui/StreamingSettingsDialog.h"
#include "config/CemuConfig.h"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statbox.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/spinctrl.h>
#include <wx/textctrl.h>
#include <wx/button.h>

wxBEGIN_EVENT_TABLE(StreamingSettingsDialog, wxDialog)
	EVT_BUTTON(wxID_OK, StreamingSettingsDialog::OnOK)
	EVT_BUTTON(wxID_CANCEL, StreamingSettingsDialog::OnCancel)
	EVT_CHOICE(wxID_ANY, StreamingSettingsDialog::OnEncoderChanged)
wxEND_EVENT_TABLE()

StreamingSettingsDialog::StreamingSettingsDialog(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Streaming Settings"), wxDefaultPosition, wxSize(450, 400))
{
	auto* sizer = new wxBoxSizer(wxVERTICAL);

	auto* groupBox = new wxStaticBox(this, wxID_ANY, _("Stream Settings"));
	auto* groupSizer = new wxStaticBoxSizer(wxVERTICAL, groupBox);

	// Enable streaming
	m_streamingEnabled = new wxCheckBox(groupBox, wxID_ANY, _("Enable Streaming"));
	groupSizer->Add(m_streamingEnabled, 0, wxALL, 5);

	// Encoder
	auto* encoderRow = new wxBoxSizer(wxHORIZONTAL);
	encoderRow->Add(new wxStaticText(groupBox, wxID_ANY, _("Encoder:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	m_encoderCombo = new wxChoice(groupBox, wxID_ANY);
	m_encoderCombo->Append(_("Auto (VAAPI then x264)"));
	m_encoderCombo->Append(_("VAAPI"));
	m_encoderCombo->Append(_("VAAPI Low Power"));
	m_encoderCombo->Append(_("x264 (Software)"));
	m_encoderCombo->Append(_("OpenH264 (Software)"));
	encoderRow->Add(m_encoderCombo, 1, wxEXPAND);
	groupSizer->Add(encoderRow, 0, wxEXPAND | wxALL, 5);

	// Bitrate / QP
	auto* bitrateRow = new wxBoxSizer(wxHORIZONTAL);
	bitrateRow->Add(new wxStaticText(groupBox, wxID_ANY, _("Bitrate (kbps):")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	m_bitrateSpin = new wxSpinCtrl(groupBox, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 100, 100000, 4000);
	bitrateRow->Add(m_bitrateSpin, 1, wxEXPAND);
	groupSizer->Add(bitrateRow, 0, wxEXPAND | wxALL, 5);

	// GPU Device
	auto* gpuRow = new wxBoxSizer(wxHORIZONTAL);
	gpuRow->Add(new wxStaticText(groupBox, wxID_ANY, _("GPU Device:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	m_gpuDeviceText = new wxTextCtrl(groupBox, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
	m_gpuDeviceText->SetHint(_("/dev/dri/renderD128 (empty=default)"));
	gpuRow->Add(m_gpuDeviceText, 1, wxEXPAND);
	groupSizer->Add(gpuRow, 0, wxEXPAND | wxALL, 5);

	// Target IP
	auto* ipRow = new wxBoxSizer(wxHORIZONTAL);
	ipRow->Add(new wxStaticText(groupBox, wxID_ANY, _("Target IP:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	m_targetIPText = new wxTextCtrl(groupBox, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
	m_targetIPText->SetHint(_("192.168.1.100"));
	ipRow->Add(m_targetIPText, 1, wxEXPAND);
	groupSizer->Add(ipRow, 0, wxEXPAND | wxALL, 5);

	// Target Port
	auto* portRow = new wxBoxSizer(wxHORIZONTAL);
	portRow->Add(new wxStaticText(groupBox, wxID_ANY, _("Target Port:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
	m_targetPortSpin = new wxSpinCtrl(groupBox, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 5000);
	portRow->Add(m_targetPortSpin, 1, wxEXPAND);
	groupSizer->Add(portRow, 0, wxEXPAND | wxALL, 5);

	sizer->Add(groupSizer, 1, wxEXPAND | wxALL, 10);

	// OK/Cancel buttons
	auto* btnSizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
	sizer->Add(btnSizer, 0, wxEXPAND | wxALL, 5);

	SetSizer(sizer);
	Layout();

	LoadSettings();
	UpdateEncoderControls();
}

StreamingSettingsDialog::~StreamingSettingsDialog() = default;

void StreamingSettingsDialog::LoadSettings()
{
	CemuConfig& cfg = GetConfig();
	m_streamingEnabled->SetValue(cfg.streaming_enabled.GetValue());
	m_encoderCombo->SetSelection(cfg.streaming_encoder.GetValue());
	m_bitrateSpin->SetValue(cfg.streaming_bitrate.GetValue());
	m_gpuDeviceText->SetValue(wxString::FromUTF8(cfg.streaming_gpu_device.GetValue()));
	m_targetIPText->SetValue(wxString::FromUTF8(cfg.streaming_target_ip.GetValue()));
	m_targetPortSpin->SetValue(cfg.streaming_target_port.GetValue());
}

void StreamingSettingsDialog::SaveSettings()
{
	CemuConfig& cfg = GetConfig();
	cfg.streaming_enabled = m_streamingEnabled->IsChecked();
	cfg.streaming_encoder = m_encoderCombo->GetSelection();
	cfg.streaming_bitrate = m_bitrateSpin->GetValue();
	cfg.streaming_gpu_device = m_gpuDeviceText->GetValue().ToStdString();
	cfg.streaming_target_ip = m_targetIPText->GetValue().ToStdString();
	cfg.streaming_target_port = m_targetPortSpin->GetValue();
	GetConfigHandle().Save();
}

void StreamingSettingsDialog::UpdateEncoderControls()
{
	const int sel = m_encoderCombo->GetSelection();
	const bool isVAAPI = (sel == 1 || sel == 2); // VAAPI or VAAPI Low Power

	if (isVAAPI)
	{
		m_bitrateSpin->SetRange(1, 51);
		m_bitrateSpin->SetValue(22);
	}
	else
	{
		m_bitrateSpin->SetRange(100, 100000);
		m_bitrateSpin->SetValue(4000);
	}
}

void StreamingSettingsDialog::OnOK(wxCommandEvent& event)
{
	SaveSettings();
	EndModal(wxID_OK);
}

void StreamingSettingsDialog::OnCancel(wxCommandEvent& event)
{
	EndModal(wxID_CANCEL);
}

void StreamingSettingsDialog::OnEncoderChanged(wxCommandEvent& event)
{
	UpdateEncoderControls();
}
