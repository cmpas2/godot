/**************************************************************************/
/*  ai_assistant_plugin.cpp                                               */
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

#include "ai_assistant_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/http_client.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "editor/docks/editor_dock.h"
#include "editor/docks/editor_dock_manager.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/text_edit.h"
#include "scene/main/http_request.h"

static constexpr int MAX_TOOL_ROUNDS = 12;
static constexpr uint64_t MAX_FILE_SIZE = 1024 * 1024;

void AIAssistantPlugin::_append_message(const String &p_role, const String &p_text) {
	transcript->append_text("[b]" + p_role.xml_escape() + ":[/b] " + p_text.xml_escape() + "\n\n");
	transcript->scroll_to_line(transcript->get_line_count());
}

void AIAssistantPlugin::_set_busy(bool p_busy) {
	busy = p_busy;
	send_button->set_disabled(p_busy);
	stop_button->set_disabled(!p_busy || provider->get_selected_id() == 1);
	prompt->set_editable(!p_busy);
}

void AIAssistantPlugin::_provider_changed(int p_index) {
	const bool use_api = provider->get_item_id(p_index) == 0;
	api_key->set_visible(use_api);
	api_key->set_editable(use_api);
	model->set_placeholder(use_api ? TTRC("API model") : TTRC("Codex model (optional)"));
	if (use_api && model->get_text().is_empty()) {
		model->set_text("gpt-5.4");
	} else if (!use_api && model->get_text() == "gpt-5.4") {
		model->clear();
	}
}

String AIAssistantPlugin::_safe_project_path(const String &p_path) const {
	String path = p_path.strip_edges().replace("\\", "/");
	if (path.begins_with("res://")) {
		path = path.trim_prefix("res://");
	}
	path = path.simplify_path();
	if (path.is_empty() || path.is_absolute_path() || path == ".." || path.begins_with("../") || path.contains("/../")) {
		return String();
	}
	return "res://" + path;
}

Dictionary AIAssistantPlugin::_read_file(const String &p_path) const {
	Dictionary result;
	const String path = _safe_project_path(p_path);
	if (path.is_empty()) {
		result["error"] = "Path must be a file inside res://.";
		return result;
	}
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) {
		result["error"] = "Unable to open " + path + ".";
		return result;
	}
	if (file->get_length() > MAX_FILE_SIZE) {
		result["error"] = "File is larger than the 1 MiB assistant limit.";
		return result;
	}
	result["path"] = path;
	result["content"] = file->get_as_text();
	return result;
}

Dictionary AIAssistantPlugin::_write_file(const String &p_path, const String &p_content) {
	Dictionary result;
	const String path = _safe_project_path(p_path);
	if (path.is_empty()) {
		result["error"] = "Path must be a file inside res://.";
		return result;
	}
	if (p_content.to_utf8_buffer().size() > MAX_FILE_SIZE) {
		result["error"] = "Content is larger than the 1 MiB assistant limit.";
		return result;
	}
	const String base_dir = path.get_base_dir();
	if (!DirAccess::dir_exists_absolute(base_dir)) {
		const Error mkdir_error = DirAccess::make_dir_recursive_absolute(base_dir);
		if (mkdir_error != OK) {
			result["error"] = "Unable to create directory " + base_dir + ".";
			return result;
		}
	}
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_null()) {
		result["error"] = "Unable to write " + path + ".";
		return result;
	}
	file->store_string(p_content);
	result["path"] = path;
	result["bytes_written"] = p_content.to_utf8_buffer().size();
	_append_message(TTR("Tool"), vformat(TTR("Wrote %d bytes to %s"), (int)result["bytes_written"], path));
	EditorFileSystem::get_singleton()->scan_changes();
	return result;
}

Dictionary AIAssistantPlugin::_list_files(const String &p_path) const {
	Dictionary result;
	const String path = _safe_project_path(p_path.is_empty() ? "." : p_path);
	if (path.is_empty()) {
		result["error"] = "Path must be a directory inside res://.";
		return result;
	}
	Ref<DirAccess> dir = DirAccess::open(path);
	if (dir.is_null()) {
		result["error"] = "Unable to open directory " + path + ".";
		return result;
	}
	PackedStringArray entries;
	dir->list_dir_begin();
	for (String name = dir->get_next(); !name.is_empty() && entries.size() < 500; name = dir->get_next()) {
		if (name.begins_with(".")) {
			continue;
		}
		entries.push_back(name + (dir->current_is_dir() ? "/" : ""));
	}
	dir->list_dir_end();
	entries.sort();
	result["path"] = path;
	result["entries"] = entries;
	return result;
}

Dictionary AIAssistantPlugin::_run_tool(const String &p_name, const Dictionary &p_arguments) {
	if (p_name == "read_file") {
		return _read_file(p_arguments.get("path", ""));
	}
	if (p_name == "write_file") {
		return _write_file(p_arguments.get("path", ""), p_arguments.get("content", ""));
	}
	if (p_name == "list_files") {
		return _list_files(p_arguments.get("path", "."));
	}
	Dictionary result;
	result["error"] = "Unknown tool: " + p_name;
	return result;
}

Array AIAssistantPlugin::_tool_definitions() const {
	Array tools;
	auto add_path_tool = [&tools](const String &p_name, const String &p_description) {
		Dictionary properties;
		properties["path"] = Dictionary{ { "type", "string" }, { "description", "Path relative to res://" } };
		Dictionary parameters{ { "type", "object" }, { "properties", properties }, { "required", Array{ "path" } }, { "additionalProperties", false } };
		tools.push_back(Dictionary{ { "type", "function" }, { "name", p_name }, { "description", p_description }, { "parameters", parameters }, { "strict", true } });
	};
	add_path_tool("read_file", "Read a UTF-8 text file in the Godot project.");
	add_path_tool("list_files", "List the direct children of a directory in the Godot project. Use repeated calls to explore subdirectories.");
	Dictionary write_properties;
	write_properties["path"] = Dictionary{ { "type", "string" }, { "description", "Path relative to res://" } };
	write_properties["content"] = Dictionary{ { "type", "string" }, { "description", "Complete new UTF-8 file contents" } };
	Dictionary write_parameters{ { "type", "object" }, { "properties", write_properties }, { "required", Array{ "path", "content" } }, { "additionalProperties", false } };
	tools.push_back(Dictionary{ { "type", "function" }, { "name", "write_file" }, { "description", "Create or replace a text file in the Godot project. Read existing files before replacing them." }, { "parameters", write_parameters }, { "strict", true } });
	return tools;
}

Error AIAssistantPlugin::_start_request(const Dictionary &p_payload) {
	PackedStringArray headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Authorization: Bearer " + api_key->get_text());
	const Error error = request->request("https://api.openai.com/v1/responses", headers, HTTPClient::METHOD_POST, JSON::stringify(p_payload));
	if (error != OK) {
		_append_message(TTR("Error"), vformat(TTR("Could not start the request (error %d)."), error));
		_set_busy(false);
	}
	return error;
}

void AIAssistantPlugin::_send_prompt() {
	const String text = prompt->get_text().strip_edges();
	if (text.is_empty() || busy) {
		return;
	}
	if (provider->get_selected_id() == 0 && api_key->get_text().is_empty()) {
		_append_message(TTR("Error"), TTR("Enter an OpenAI API key first. The key is kept only for this editor session."));
		return;
	}
	_append_message(TTR("You"), text);
	prompt->clear();
	previous_response_id = String();
	tool_rounds = 0;
	_set_busy(true);
	if (provider->get_selected_id() == 1) {
		_append_message(TTR("System"), TTR("Running Codex CLI using your local ChatGPT sign-in. Codex can edit this project directly."));
		codex_prompt = text;
		codex_model = model->get_text().strip_edges();
		codex_thread.start(&AIAssistantPlugin::_codex_thread_callback, this);
		if (!codex_thread.is_started()) {
			_append_message(TTR("Error"), TTR("Could not start the Codex worker thread."));
			_set_busy(false);
		}
		return;
	}

	Dictionary payload;
	payload["model"] = model->get_text().strip_edges();
	payload["instructions"] = "You are a coding agent inside the Godot editor. Inspect the project with list_files and read_file, then implement the user's request with write_file. Work only in res://. Preserve existing behavior, use idiomatic Godot 4 APIs, and finish with a concise summary of edits. Do not claim an edit unless the tool succeeded.";
	payload["input"] = text;
	payload["tools"] = _tool_definitions();
	_start_request(payload);
}

void AIAssistantPlugin::_codex_thread_callback(void *p_userdata) {
	static_cast<AIAssistantPlugin *>(p_userdata)->_run_codex();
}

void AIAssistantPlugin::_run_codex() {
	List<String> arguments;
	arguments.push_back("exec");
	arguments.push_back("--full-auto");
	arguments.push_back("--skip-git-repo-check");
	arguments.push_back("--color");
	arguments.push_back("never");
	arguments.push_back("-C");
	arguments.push_back(ProjectSettings::get_singleton()->globalize_path("res://"));
	if (!codex_model.is_empty()) {
		arguments.push_back("--model");
		arguments.push_back(codex_model);
	}
	arguments.push_back(codex_prompt);

	String output;
	int exit_code = -1;
	const Error error = OS::get_singleton()->execute("codex", arguments, &output, &exit_code, true);
	callable_mp(this, &AIAssistantPlugin::_codex_finished).call_deferred(output, exit_code, error);
}

void AIAssistantPlugin::_codex_finished(const String &p_output, int p_exit_code, Error p_error) {
	if (codex_thread.is_started()) {
		codex_thread.wait_to_finish();
	}
	if (p_error != OK) {
		_append_message(TTR("Error"), TTR("Could not launch Codex CLI. Install Codex, run `codex login`, and make sure `codex` is available on PATH."));
	} else if (p_exit_code != 0) {
		_append_message(TTR("Error"), vformat(TTR("Codex exited with code %d:\n%s"), p_exit_code, p_output));
	} else {
		_append_message(TTR("Codex"), p_output.strip_edges().is_empty() ? TTR("Finished editing the project.") : p_output.strip_edges());
		EditorFileSystem::get_singleton()->scan_changes();
	}
	_set_busy(false);
}

void AIAssistantPlugin::_stop() {
	if (!busy) {
		return;
	}
	request->cancel_request();
	_set_busy(false);
	_append_message(TTR("System"), TTR("Request stopped."));
}

void AIAssistantPlugin::_request_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	if (!busy) {
		return;
	}
	const String body = String::utf8(reinterpret_cast<const char *>(p_body.ptr()), p_body.size());
	if (p_result != HTTPRequest::RESULT_SUCCESS || p_response_code < 200 || p_response_code >= 300) {
		Variant parsed_error = JSON::parse_string(body);
		String message = body.left(1000);
		if (parsed_error.get_type() == Variant::DICTIONARY) {
			Dictionary error_body = parsed_error;
			Dictionary error = error_body.get("error", Dictionary());
			message = error.get("message", message);
		}
		_append_message(TTR("Error"), vformat(TTR("OpenAI request failed (HTTP %d): %s"), p_response_code, message));
		_set_busy(false);
		return;
	}

	Variant parsed = JSON::parse_string(body);
	if (parsed.get_type() != Variant::DICTIONARY) {
		_append_message(TTR("Error"), TTR("OpenAI returned an invalid JSON response."));
		_set_busy(false);
		return;
	}
	Dictionary response = parsed;
	previous_response_id = response.get("id", "");
	Array outputs = response.get("output", Array());
	Array tool_outputs;
	String answer;
	for (const Variant &output_variant : outputs) {
		Dictionary output = output_variant;
		const String type = output.get("type", "");
		if (type == "function_call") {
			Dictionary arguments;
			Variant parsed_arguments = JSON::parse_string(output.get("arguments", "{}"));
			if (parsed_arguments.get_type() == Variant::DICTIONARY) {
				arguments = parsed_arguments;
			}
			const Dictionary tool_result = _run_tool(output.get("name", ""), arguments);
			tool_outputs.push_back(Dictionary{ { "type", "function_call_output" }, { "call_id", output.get("call_id", "") }, { "output", JSON::stringify(tool_result) } });
		} else if (type == "message") {
			Array content = output.get("content", Array());
			for (const Variant &content_variant : content) {
				Dictionary part = content_variant;
				if (part.get("type", "") == "output_text") {
					answer += String(part.get("text", ""));
				}
			}
		}
	}

	if (!tool_outputs.is_empty()) {
		tool_rounds++;
		if (tool_rounds >= MAX_TOOL_ROUNDS) {
			_append_message(TTR("Error"), TTR("Stopped after 12 tool rounds. Refine the request and try again."));
			_set_busy(false);
			return;
		}
		Dictionary payload;
		payload["model"] = model->get_text().strip_edges();
		payload["previous_response_id"] = previous_response_id;
		payload["input"] = tool_outputs;
		payload["tools"] = _tool_definitions();
		_start_request(payload);
		return;
	}

	_append_message(TTR("Assistant"), answer.is_empty() ? TTR("Finished.") : answer);
	_set_busy(false);
}

AIAssistantPlugin::AIAssistantPlugin() {
	dock = memnew(EditorDock);
	dock->set_title(TTRC("AI Assistant"));
	dock->set_name("AIAssistant");
	dock->set_layout_key("ai_assistant");
	dock->set_icon_name("Script");
	dock->set_default_slot(EditorDock::DOCK_SLOT_RIGHT_BL);
	dock->set_available_layouts(EditorDock::DOCK_LAYOUT_ALL);

	VBoxContainer *root = memnew(VBoxContainer);
	dock->add_child(root);

	Label *notice = memnew(Label(TTRC("AI can modify files in this project. Review changes in version control.")));
	notice->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	root->add_child(notice);

	provider = memnew(OptionButton);
	provider->add_item(TTRC("OpenAI API key"), 0);
	provider->add_item(TTRC("ChatGPT account via Codex CLI"), 1);
	provider->select(1);
	provider->connect(SceneStringName(item_selected), callable_mp(this, &AIAssistantPlugin::_provider_changed));
	provider->set_tooltip_text(TTRC("The Codex CLI option uses the account authenticated by `codex login`; a ChatGPT subscription does not supply an API key."));
	root->add_child(provider);

	api_key = memnew(LineEdit);
	api_key->set_placeholder(TTRC("OpenAI API key (or set OPENAI_API_KEY)"));
	api_key->set_secret(true);
	api_key->set_text(OS::get_singleton()->get_environment("OPENAI_API_KEY"));
	root->add_child(api_key);

	model = memnew(LineEdit);
	model->set_placeholder(TTRC("Model"));
	root->add_child(model);
	_provider_changed(provider->get_selected());

	transcript = memnew(RichTextLabel);
	transcript->set_bbcode_enabled(true);
	transcript->set_fit_content(false);
	transcript->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	transcript->set_custom_minimum_size(Size2(280, 180) * EDSCALE);
	root->add_child(transcript);

	prompt = memnew(TextEdit);
	prompt->set_placeholder(TTRC("Describe what you want to build or change…"));
	prompt->set_custom_minimum_size(Size2(0, 100) * EDSCALE);
	prompt->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	root->add_child(prompt);

	HBoxContainer *actions = memnew(HBoxContainer);
	root->add_child(actions);
	send_button = memnew(Button(TTRC("Send and Edit")));
	send_button->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	send_button->connect(SceneStringName(pressed), callable_mp(this, &AIAssistantPlugin::_send_prompt));
	actions->add_child(send_button);
	stop_button = memnew(Button(TTRC("Stop")));
	stop_button->set_disabled(true);
	stop_button->connect(SceneStringName(pressed), callable_mp(this, &AIAssistantPlugin::_stop));
	actions->add_child(stop_button);

	request = memnew(HTTPRequest);
	request->set_timeout(120);
	request->set_body_size_limit(8 * 1024 * 1024);
	request->connect("request_completed", callable_mp(this, &AIAssistantPlugin::_request_completed));
	add_child(request);

	EditorDockManager::get_singleton()->add_dock(dock);
}

AIAssistantPlugin::~AIAssistantPlugin() {
	if (codex_thread.is_started()) {
		codex_thread.wait_to_finish();
	}
	if (dock) {
		EditorDockManager::get_singleton()->remove_dock(dock);
		memdelete(dock);
	}
}
