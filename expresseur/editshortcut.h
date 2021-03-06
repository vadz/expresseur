// update : 31 / 10 / 2016 10 : 00
#ifndef DEF_EDITSHORTCUT

#define DEF_EDITSHORTCUT


class editshortcut
	: public wxDialog
{

public:
	editshortcut(wxWindow *parent, wxWindowID id, const wxString &title,
		wxString *lname,
		wxString *laction, wxArrayString nameAction,
		wxString *lkey,
		wxString *ltrack, wxArrayString nameTrack,
		wxString *lchannel, wxArrayString nameChannel,
		wxString *levent, wxArrayString nameEvent,
		wxString *lmin, wxArrayString nameValueMin,
		wxString *lmax, wxArrayString nameValueMax,
		wxString *lparam,
		wxString *lstopOnMatch, wxArrayString namestopOnMatch
		);
	~editshortcut();

	void OnMidi(wxCommandEvent& event);
	void scanMidi();

private:
	wxListBox *listMidi;

	wxChoice *fEvent;
	wxChoice *fTdevice;
	wxChoice *fTchannel;
	wxChoice *fMin;
	wxDECLARE_EVENT_TABLE();

};

#endif