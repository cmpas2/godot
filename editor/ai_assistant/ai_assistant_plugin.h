/**************************************************************************/
/*  ai_assistant_plugin.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             Godot Engine                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "editor/plugins/editor_plugin.h"

class Button;
class EditorDock;
class HTTPRequest;
class LineEdit;
class OptionButton;
class RichTextLabel;
class TextEdit;

class AIAssistantPlugin : public EditorPlugin {
	GDCLASS(AIAssistantPlugin, EditorPlugin);

	EditorDock *dock = nullptr;
	HTTPRequest *request = nullptr;
	LineEdit *api_key = nullptr;
	LineEdit *model = nullptr;
	LineEdit *codex_path = nullptr;
	OptionButton *provider = nullptr;
	RichTextLabel *transcript = nullptr;
	TextEdit *prompt = nullptr;
	Button *send_button = nullptr;
	Button *stop_button = nullptr;
	Button *apply_button = nullptr;
	Button *discard_button = nullptr;
	Button *undo_button = nullptr;

	String previous_response_id;
	int tool_rounds = 0;
	bool busy = false;
	Dictionary codex_process;
	String codex_output;
	uint64_t codex_started_at = 0;
	HashMap<String, String> staged_files;
	HashMap<String, String> rollback_files;
	HashSet<String> rollback_created_files;

	void _provider_changed(int p_index);
	void _send_prompt();
	void _start_codex(const String &p_prompt);
	void _poll_codex();
	void _finish_codex(int p_exit_code);
	void _stop();
	void _apply_staged_files();
	void _discard_staged_files();
	void _undo_last_apply();
	void _update_staging_controls();
	void _load_history();
	String _validate_staged_content(const String &p_path, const String &p_content) const;
	void _request_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	Error _start_request(const Dictionary &p_payload);
	Array _tool_definitions() const;
	Dictionary _run_tool(const String &p_name, const Dictionary &p_arguments);
	Dictionary _read_file(const String &p_path) const;
	Dictionary _write_file(const String &p_path, const String &p_content);
	Dictionary _list_files(const String &p_path) const;
	String _safe_project_path(const String &p_path) const;
	void _append_message(const String &p_role, const String &p_text);
	void _set_busy(bool p_busy);

protected:
	void _notification(int p_what);

public:
	virtual String get_plugin_name() const override { return "AI Assistant"; }

	AIAssistantPlugin();
	~AIAssistantPlugin();
};
