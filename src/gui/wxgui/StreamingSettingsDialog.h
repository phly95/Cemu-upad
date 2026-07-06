#pragma once

#include <wx/dialog.h>

class wxCheckBox;
class wxChoice;
class wxSpinCtrl;
class wxTextCtrl;

class StreamingSettingsDialog : public wxDialog
{
public:
	StreamingSettingsDialog(wxWindow* parent);
	~StreamingSettingsDialog();

	void OnOK(wxCommandEvent& event);
	void OnCancel(wxCommandEvent& event);
	void OnEncoderChanged(wxCommandEvent& event);

private:
	void LoadSettings();
	void SaveSettings();
	void UpdateEncoderControls();

	wxCheckBox* m_streamingEnabled;
	wxChoice* m_encoderCombo;
	wxSpinCtrl* m_bitrateSpin;
	wxTextCtrl* m_gpuDeviceText;
	wxTextCtrl* m_targetIPText;
	wxSpinCtrl* m_targetPortSpin;
	wxCheckBox* m_useQP; // shown for VAAPI encoders

=wxDECLARE_EVENT_TABLE();
};
