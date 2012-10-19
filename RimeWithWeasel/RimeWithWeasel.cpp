#include "stdafx.h"
#include <RimeWithWeasel.h>
#include <WeaselUtility.h>
#include <WeaselVersion.h>
#include <list>
#include <set>
#include <string>
#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>

#define GLOG_NO_ABBREVIATED_SEVERITIES
#pragma warning(disable : 4244)
#include <glog/logging.h>
#pragma warning(default : 4244)

#pragma warning(disable: 4005)
#include <rime_api.h>
#pragma warning(default: 4005)

int expand_ibus_modifier(int m)
{
	return (m & 0xff) | ((m & 0xff00) << 16);
}

RimeWithWeaselHandler::RimeWithWeaselHandler(weasel::UI *ui)
	: m_ui(ui), m_active_session(0), m_client_caps(0), m_disabled(true)
{
}

RimeWithWeaselHandler::~RimeWithWeaselHandler()
{
}

void _UpdateUIStyle(RimeConfig* config, weasel::UI* ui);
void _LoadAppOptions(RimeConfig* config, AppOptionsByAppName& app_options);

void RimeWithWeaselHandler::Initialize()
{
	m_disabled = _IsDeployerRunning();
	if (m_disabled)
	{
		 return;
	}
	LOG(INFO) << "Initializing la rime.";
	RimeTraits weasel_traits;
	weasel_traits.shared_data_dir = weasel_shared_data_dir();
	weasel_traits.user_data_dir = weasel_user_data_dir();
	const int len = 20;
	char utf8_str[len];
	memset(utf8_str, 0, sizeof(utf8_str));
	WideCharToMultiByte(CP_UTF8, 0, WEASEL_IME_NAME, -1, utf8_str, len - 1, NULL, NULL);
	weasel_traits.distribution_name = utf8_str;
	weasel_traits.distribution_code_name = WEASEL_CODE_NAME;
	weasel_traits.distribution_version = WEASEL_VERSION;
	RimeInitialize(&weasel_traits);
	if (RimeStartMaintenanceOnWorkspaceChange())
	{
		m_disabled = true;
	}

	RimeConfig config = { NULL };
	if (RimeConfigOpen("weasel", &config))
	{
		_UpdateUIStyle(&config, m_ui);
		_LoadAppOptions(&config, m_app_options);
		RimeConfigClose(&config);
	}
}

void RimeWithWeaselHandler::Finalize()
{
	m_active_session = 0;
    m_client_caps = 0;
	m_disabled = true;
	LOG(INFO) << "Finalizing la rime.";
	RimeFinalize();
}

UINT RimeWithWeaselHandler::FindSession(UINT session_id)
{
	if (m_disabled) return 0;
	bool found = RimeFindSession(session_id);
	DLOG(INFO) << "Find session: session_id = " << session_id << ", found = " << found;
	return found ? session_id : 0;
}

UINT RimeWithWeaselHandler::AddSession(LPWSTR buffer)
{
	if (m_disabled)
	{
		DLOG(INFO) << "Trying to resume service.";
		EndMaintenance();
		if (m_disabled) return 0;
	}
	UINT session_id = RimeCreateSession();
	DLOG(INFO) << "Add session: created session_id = " << session_id;
	// show soft cursor on weasel panel
	RimeSetOption(session_id, "soft_cursor", Bool(!m_ui->style().inline_preedit));
	// show session's welcome message :-) if any
	m_client_caps = 0;
	_ReadClientInfo(session_id, buffer);
	_UpdateUI(session_id);
	m_active_session = session_id;
	return session_id;
}

UINT RimeWithWeaselHandler::RemoveSession(UINT session_id)
{
	if (m_ui) m_ui->Hide();
	if (m_disabled) return 0;
	DLOG(INFO) << "Remove session: session_id = " << session_id;
	// TODO: force committing? otherwise current composition would be lost
	RimeDestroySession(session_id);
	m_active_session = 0;
    m_client_caps = 0;
	return 0;
}

namespace ibus
{
	enum Keycode
	{
		Escape = 0xFF1B,
	};
}

BOOL RimeWithWeaselHandler::ProcessKeyEvent(weasel::KeyEvent keyEvent, UINT session_id, LPWSTR buffer)
{
	DLOG(INFO) << "Process key event: keycode = " << keyEvent.keycode << ", mask = " << keyEvent.mask
		 << ", session_id = " << session_id;
	if (m_disabled) return FALSE;
	bool handled = RimeProcessKey(session_id, keyEvent.keycode, expand_ibus_modifier(keyEvent.mask));
	_Respond(session_id, buffer);
	_UpdateUI(session_id);
	m_active_session = session_id;
	return (BOOL)handled;
}

void RimeWithWeaselHandler::FocusIn(DWORD client_caps, UINT session_id)
{
	DLOG(INFO) << "Focus in: session_id = " << session_id << ", client_caps = " << client_caps;
	if (m_disabled) return;
    m_client_caps = client_caps;
	_UpdateUI(session_id);
	m_active_session = session_id;
}

void RimeWithWeaselHandler::FocusOut(DWORD param, UINT session_id)
{
	DLOG(INFO) << "Focus out: session_id = " << session_id;
	if (m_ui) m_ui->Hide();
	m_active_session = 0;
    m_client_caps = 0;
}

void RimeWithWeaselHandler::UpdateInputPosition(RECT const& rc, UINT session_id)
{
	DLOG(INFO) << "Update input position: (" << rc.left << ", " << rc.top
		<< "), session_id = " << session_id << ", m_active_session = " << m_active_session;
	if (m_ui) m_ui->UpdateInputPosition(rc);
	if (m_disabled) return;
	if (m_active_session != session_id)
	{
        m_client_caps = 0;
		_UpdateUI(session_id);
		m_active_session = session_id;
	}
}

void RimeWithWeaselHandler::_ReadClientInfo(UINT session_id, LPWSTR buffer)
{
    std::string app_name;
	wbufferstream bs(buffer, WEASEL_IPC_BUFFER_LENGTH);
	std::wstring line;
	while (bs.good())
	{
		std::getline(bs, line);
		if (!bs.good())
			break;
		// file ends
		if (line == L".")
			break;
		const std::wstring kClientKey = L"session.client=";
		if (boost::starts_with(line, kClientKey))
		{
			app_name = wcstoutf8(line.substr(kClientKey.length()).c_str());
			boost::to_lower(app_name);
		}
	}
    // set app specific options
	if (!app_name.empty())
	{
		RimeSetProperty(session_id, "app", app_name.c_str());

		if (m_app_options.find(app_name) != m_app_options.end())
		{
			AppOptions& options(m_app_options[app_name]);
			for (AppOptions::const_iterator it = options.begin(); it != options.end(); ++it)
			{
				DLOG(INFO) << "set app option: " << it->first << " = " << it->second;
				RimeSetOption(session_id, it->first.c_str(), Bool(it->second));
			}
		}
	}
}

void RimeWithWeaselHandler::StartMaintenance()
{
	Finalize();
	_UpdateUI(0);
}

void RimeWithWeaselHandler::EndMaintenance()
{
	if (m_disabled)
	{
		Initialize();
		_UpdateUI(0);
	}
}

bool RimeWithWeaselHandler::_IsDeployerRunning()
{
	HANDLE hMutex = CreateMutex(NULL, TRUE, L"WeaselDeployerMutex");
	bool deployer_detected = hMutex && GetLastError() == ERROR_ALREADY_EXISTS;
	if (hMutex)
	{
		CloseHandle(hMutex);
	}
	return deployer_detected;
}

void RimeWithWeaselHandler::_UpdateUI(UINT session_id)
{
	weasel::Status weasel_status;
	weasel::Context weasel_context;
	if (session_id == 0)
		weasel_status.disabled = m_disabled;

	RimeStatus status = {0};
	RIME_STRUCT_INIT(RimeStatus, status);
	if (RimeGetStatus(session_id, &status))
	{
		weasel_status.ascii_mode = status.is_ascii_mode;
		weasel_status.composing = status.is_composing;
		weasel_status.disabled = status.is_disabled;
		RimeFreeStatus(&status);
	}

	RimeContext ctx = {0};
	RIME_STRUCT_INIT(RimeContext, ctx);
	if (RimeGetContext(session_id, &ctx))
	{
		if (ctx.composition.length > 0)
		{
			weasel_context.preedit.str = utf8towcs(ctx.composition.preedit);
			if (ctx.composition.sel_start < ctx.composition.sel_end)
			{
				weasel::TextAttribute attr;
				attr.type = weasel::HIGHLIGHTED;
				attr.range.start = utf8towcslen(ctx.composition.preedit, ctx.composition.sel_start);
				attr.range.end = utf8towcslen(ctx.composition.preedit, ctx.composition.sel_end);
			
				weasel_context.preedit.attributes.push_back(attr);
			}
		}
		if (ctx.menu.num_candidates)
		{
			weasel::CandidateInfo &cinfo(weasel_context.cinfo);
			cinfo.candies.resize(ctx.menu.num_candidates);
			cinfo.comments.resize(ctx.menu.num_candidates);
			for (int i = 0; i < ctx.menu.num_candidates; ++i)
			{
				cinfo.candies[i].str = utf8towcs(ctx.menu.candidates[i].text);
				if (ctx.menu.candidates[i].comment)
				{
					cinfo.comments[i].str = utf8towcs(ctx.menu.candidates[i].comment);
				}
			}
			cinfo.highlighted = ctx.menu.highlighted_candidate_index;
			cinfo.currentPage = ctx.menu.page_no;
			cinfo.labels = ctx.menu.select_keys;
		}
		RimeFreeContext(&ctx);
	}

	if (!m_ui) return;
	if (weasel_status.composing)
	{
		m_ui->Update(weasel_context, weasel_status);
		m_ui->Show();
	}
	else
	{
		m_ui->Hide();
		m_ui->Update(weasel_context, weasel_status);
	}
}

bool RimeWithWeaselHandler::_Respond(UINT session_id, LPWSTR buffer)
{
	std::set<std::string> actions;
	std::list<std::string> messages;

	// extract information

	RimeCommit commit = {0};
	if (RimeGetCommit(session_id, &commit))
	{
		actions.insert("commit");
		messages.push_back(boost::str(boost::format("commit=%s\n") % commit.text));
		RimeFreeCommit(&commit);
	}
	
	bool is_composing;
	RimeStatus status = {0};
	RIME_STRUCT_INIT(RimeStatus, status);
	if (RimeGetStatus(session_id, &status))
	{
		is_composing = status.is_composing;
		actions.insert("status");
		messages.push_back(boost::str(boost::format("status.ascii_mode=%d\n") % status.is_ascii_mode));
		messages.push_back(boost::str(boost::format("status.composing=%d\n") % status.is_composing));
		messages.push_back(boost::str(boost::format("status.disabled=%d\n") % status.is_disabled));
		RimeFreeStatus(&status);
	}
	
	RimeContext ctx = {0};
	RIME_STRUCT_INIT(RimeContext, ctx);
	if (RimeGetContext(session_id, &ctx))
	{
		if (is_composing)
		{
			actions.insert("ctx");
			messages.push_back(boost::str(boost::format("ctx.preedit=%s\n") % ctx.composition.preedit));
			if (ctx.composition.sel_start <= ctx.composition.sel_end)
			{
				messages.push_back(boost::str(boost::format("ctx.preedit.cursor=%d,%d\n") %
					utf8towcslen(ctx.composition.preedit, ctx.composition.sel_start) %
					utf8towcslen(ctx.composition.preedit, ctx.composition.sel_end)));
			}
		}
		RimeFreeContext(&ctx);
	}

	// configuration information
	actions.insert("config");
	messages.push_back(boost::str(boost::format("config.inline_preedit=%d\n") % (int) m_ui->style().inline_preedit));

	// summarize

	if (actions.empty())
	{
		messages.insert(messages.begin(), std::string("action=noop\n"));
	}
	else
	{
		std::string actionList(boost::join(actions, ","));
		messages.insert(messages.begin(), boost::str(boost::format("action=%s\n") % actionList));
	}

	messages.push_back(std::string(".\n"));

	// printing to stream

	memset(buffer, 0, WEASEL_IPC_BUFFER_SIZE);
	wbufferstream bs(buffer, WEASEL_IPC_BUFFER_LENGTH);

	BOOST_FOREACH(const std::string &msg, messages)
	{
		bs << utf8towcs(msg.c_str());
		if (!bs.good())
		{
			// response text toooo long!
			return false;
		}
	}

	return true;
}

static inline COLORREF blend_colors(COLORREF fcolor, COLORREF bcolor)
{
	return RGB(
		(GetRValue(fcolor) * 2 + GetRValue(bcolor)) / 3,
		(GetGValue(fcolor) * 2 + GetGValue(bcolor)) / 3,
		(GetBValue(fcolor) * 2 + GetBValue(bcolor)) / 3
		);
}

static void _UpdateUIStyle(RimeConfig* config, weasel::UI* ui)
{
	if (!ui) return;
	weasel::UIStyle &style(ui->style());

	const int BUF_SIZE = 99;
	char buffer[BUF_SIZE + 1];
	memset(buffer, '\0', sizeof(buffer));
	if (RimeConfigGetString(config, "style/font_face", buffer, BUF_SIZE))
	{
		style.font_face = utf8towcs(buffer);
	}
	RimeConfigGetInt(config, "style/font_point", &style.font_point);
	Bool inline_preedit = False;
	RimeConfigGetBool(config, "style/inline_preedit", &inline_preedit);
	style.inline_preedit = inline_preedit;
	Bool horizontal = False;
	RimeConfigGetBool(config, "style/horizontal", &horizontal);
	style.layout_type = horizontal ? weasel::LAYOUT_HORIZONTAL : weasel::LAYOUT_VERTICAL;
	// layout (alternative to style/horizontal)
	char layout_type[256] = {0};
	if (RimeConfigGetString(config, "style/layout/type", layout_type, sizeof(layout_type) - 1))
	{
		if (!std::strcmp(layout_type, "vertical"))
			style.layout_type = weasel::LAYOUT_VERTICAL;
		else if (!std::strcmp(layout_type, "horizontal"))
			style.layout_type = weasel::LAYOUT_HORIZONTAL;
		else
			LOG(WARNING) << "Invalid style type: " << layout_type;
	}
	RimeConfigGetInt(config, "style/layout/min_width", &style.min_width);
	RimeConfigGetInt(config, "style/layout/min_height", &style.min_height);
	RimeConfigGetInt(config, "style/layout/border", &style.border);
	RimeConfigGetInt(config, "style/layout/margin_x", &style.margin_x);
	RimeConfigGetInt(config, "style/layout/margin_y", &style.margin_y);
	RimeConfigGetInt(config, "style/layout/spacing", &style.spacing);
	RimeConfigGetInt(config, "style/layout/candidate_spacing", &style.candidate_spacing);
	RimeConfigGetInt(config, "style/layout/hilite_spacing", &style.hilite_spacing);
	RimeConfigGetInt(config, "style/layout/hilite_padding", &style.hilite_padding);
	RimeConfigGetInt(config, "style/layout/round_corner", &style.round_corner);
	// color scheme
	if (RimeConfigGetString(config, "style/color_scheme", buffer, BUF_SIZE))
	{
		std::string prefix("preset_color_schemes/");
		prefix += buffer;
		RimeConfigGetInt(config, (prefix + "/text_color").c_str(), &style.text_color);
		if (!RimeConfigGetInt(config, (prefix + "/candidate_text_color").c_str(), &style.candidate_text_color))
		{
			style.candidate_text_color = style.text_color;
		}
		RimeConfigGetInt(config, (prefix + "/back_color").c_str(), &style.back_color);
		if (!RimeConfigGetInt(config, (prefix + "/border_color").c_str(), &style.border_color))
		{
			style.border_color = style.text_color;
		}
		if (!RimeConfigGetInt(config, (prefix + "/hilited_text_color").c_str(), &style.hilited_text_color))
		{
			style.hilited_text_color = style.text_color;
		}
		if (!RimeConfigGetInt(config, (prefix + "/hilited_back_color").c_str(), &style.hilited_back_color))
		{
			style.hilited_back_color = style.back_color;
		}
		if (!RimeConfigGetInt(config, (prefix + "/hilited_candidate_text_color").c_str(), &style.hilited_candidate_text_color))
		{
			style.hilited_candidate_text_color = style.hilited_text_color;
		}
		if (!RimeConfigGetInt(config, (prefix + "/hilited_candidate_back_color").c_str(), &style.hilited_candidate_back_color))
		{
			style.hilited_candidate_back_color = style.hilited_back_color;
		}
		style.label_text_color = blend_colors(style.candidate_text_color, style.back_color);
		style.hilited_label_text_color = blend_colors(style.hilited_candidate_text_color, style.hilited_candidate_back_color);
		style.comment_text_color = style.label_text_color;
		style.hilited_comment_text_color = style.hilited_label_text_color;
		if (RimeConfigGetInt(config, (prefix + "/comment_text_color").c_str(), &style.comment_text_color))
		{
			style.hilited_comment_text_color = style.comment_text_color;
		}
		RimeConfigGetInt(config, (prefix + "/hilited_comment_text_color").c_str(), &style.hilited_comment_text_color);
	}
}

static void _LoadAppOptions(RimeConfig* config, AppOptionsByAppName& app_options)
{
	app_options.clear();
	RimeConfigIterator app_iter;
	RimeConfigIterator option_iter;
	RimeConfigBeginMap(&app_iter, config, "app_options");
	while (RimeConfigNext(&app_iter)) {
		AppOptions &options(app_options[app_iter.key]);
		RimeConfigBeginMap(&option_iter, config, app_iter.path);
		while (RimeConfigNext(&option_iter)) {
			Bool value = False;
			if (RimeConfigGetBool(config, option_iter.path, &value)) {
				options[option_iter.key] = bool(value);
			}
		}
		RimeConfigEnd(&option_iter);
	}
	RimeConfigEnd(&app_iter);
}

