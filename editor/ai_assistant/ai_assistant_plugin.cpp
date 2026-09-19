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
#include "core/os/time.h"
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
static constexpr uint64_t CODEX_TIMEOUT_MSEC = 15 * 60 * 1000;

void AIAssistantPlugin::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS && !codex_process.is_empty()) {
		_poll_codex();
	}
}

void AIAssistantPlugin::_append_message(const String &p_role, const String &p_text) {
	transcript->append_text("[b]" + p_role.xml_escape() + ":[/b] " + p_text.xml_escape() + "\n\n");
	transcript->scroll_to_line(transcript->get_line_count());
	const String history_path = "user://ai_assistant_history.jsonl";
	const FileAccess::ModeFlags mode = FileAccess::exists(history_path) ? FileAccess::READ_WRITE : FileAccess::WRITE_READ;
	Ref<FileAccess> history = FileAccess::open(history_path, mode);
	if (history.is_valid()) {
		history->seek_end();
		history->store_line(JSON::stringify(Dictionary{ { "time", Time::get_singleton()->get_datetime_string_from_system(true) }, { "role", p_role }, { "text", p_text } }));
	}
}

void AIAssistantPlugin::_load_history() {
	Ref<FileAccess> history = FileAccess::open("user://ai_assistant_history.jsonl", FileAccess::READ);
	if (history.is_null()) {
		return;
	}
	while (!history->eof_reached()) {
		const Variant entry_variant = JSON::parse_string(history->get_line());
		if (entry_variant.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = entry_variant;
		transcript->append_text("[color=gray]" + String(entry.get("time", "")).xml_escape() + "[/color] [b]" + String(entry.get("role", "")).xml_escape() + ":[/b] " + String(entry.get("text", "")).xml_escape() + "\n\n");
	}
}

void AIAssistantPlugin::_set_busy(bool p_busy) {
	busy = p_busy;
	send_button->set_disabled(p_busy);
	stop_button->set_disabled(!p_busy);
	prompt->set_editable(!p_busy);
	if (apply_button) {
		_update_staging_controls();
	}
}

void AIAssistantPlugin::_provider_changed(int p_index) {
	const bool use_api = provider->get_item_id(p_index) == 0;
	api_key->set_visible(use_api);
	api_key->set_editable(use_api);
	codex_path->set_visible(!use_api);
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
	Ref<DirAccess> directory = DirAccess::open("res://");
	if (directory.is_null()) {
		return String();
	}
	const PackedStringArray components = path.split("/", false);
	for (int i = 0; i < components.size(); i++) {
		if (directory->is_link(components[i])) {
			return String();
		}
		if (i + 1 < components.size() && directory->dir_exists(components[i]) && directory->change_dir(components[i]) != OK) {
			return String();
		}
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
	staged_files[path] = p_content;
	result["path"] = path;
	result["bytes_staged"] = p_content.to_utf8_buffer().size();
	result["status"] = "staged_for_user_review";
	_append_message(TTR("Tool"), vformat(TTR("Staged %d bytes for %s"), (int)result["bytes_staged"], path));
	_update_staging_controls();
	return result;
}

void AIAssistantPlugin::_update_staging_controls() {
	const bool has_staged = !staged_files.is_empty();
	apply_button->set_disabled(!has_staged || busy);
	discard_button->set_disabled(!has_staged || busy);
	undo_button->set_disabled((rollback_files.is_empty() && rollback_created_files.is_empty()) || busy);
	apply_button->set_text(has_staged ? vformat(TTR("Apply %d File(s)"), staged_files.size()) : TTR("Apply Changes"));
}

void AIAssistantPlugin::_discard_staged_files() {
	const int count = staged_files.size();
	staged_files.clear();
	_append_message(TTR("System"), vformat(TTR("Discarded %d staged file(s)."), count));
	_update_staging_controls();
}

String AIAssistantPlugin::_validate_staged_content(const String &p_path, const String &p_content) const {
	if (p_content.contains_char('\0')) {
		return TTR("Text files cannot contain NUL bytes.");
	}
	const String extension = p_path.get_extension().to_lower();
	if (extension == "json" && JSON::parse_string(p_content).get_type() == Variant::NIL && p_content.strip_edges() != "null") {
		return TTR("The generated JSON is invalid.");
	}
	if (extension == "tscn" && !p_content.strip_edges().begins_with("[gd_scene")) {
		return TTR("A text scene must begin with a gd_scene header.");
	}
	if (extension == "tres" && !p_content.strip_edges().begins_with("[gd_resource")) {
		return TTR("A text resource must begin with a gd_resource header.");
	}
	if (extension == "svg" && !p_content.contains("<svg")) {
		return TTR("The generated SVG does not contain an svg root element.");
	}
	return String();
}

void AIAssistantPlugin::_apply_staged_files() {
	if (busy || staged_files.is_empty()) {
		return;
	}
	rollback_files.clear();
	rollback_created_files.clear();
	Vector<String> applied;
	String failure;
	for (const KeyValue<String, String> &entry : staged_files) {
		const String path = entry.key;
		const String validation_error = _validate_staged_content(path, entry.value);
		if (!validation_error.is_empty()) {
			failure = vformat(TTR("Validation failed for %s: %s"), path, validation_error);
			break;
		}
		const String directory = path.get_base_dir();
		if (!DirAccess::dir_exists_absolute(directory) && DirAccess::make_dir_recursive_absolute(directory) != OK) {
			failure = vformat(TTR("Could not create %s."), directory);
			break;
		}
		if (FileAccess::exists(path)) {
			Ref<FileAccess> original = FileAccess::open(path, FileAccess::READ);
			if (original.is_null()) {
				failure = vformat(TTR("Could not back up %s."), path);
				break;
			}
			rollback_files[path] = original->get_as_text();
		} else {
			rollback_created_files.insert(path);
		}
		const String temporary_path = path + ".ai_assistant_tmp";
		Ref<FileAccess> temporary = FileAccess::open(temporary_path, FileAccess::WRITE);
		if (temporary.is_null()) {
			failure = vformat(TTR("Could not stage temporary file for %s."), path);
			break;
		}
		temporary->store_string(entry.value);
		temporary.unref();
		if (FileAccess::exists(path) && DirAccess::remove_absolute(path) != OK) {
			DirAccess::remove_absolute(temporary_path);
			failure = vformat(TTR("Could not replace %s."), path);
			break;
		}
		if (DirAccess::rename_absolute(temporary_path, path) != OK) {
			if (rollback_files.has(path)) {
				Ref<FileAccess> restored = FileAccess::open(path, FileAccess::WRITE);
				if (restored.is_valid()) {
					restored->store_string(rollback_files[path]);
				}
			}
			failure = vformat(TTR("Could not move the staged file into %s."), path);
			break;
		}
		applied.push_back(path);
	}
	if (!failure.is_empty()) {
		for (const String &path : applied) {
			if (rollback_created_files.has(path)) {
				DirAccess::remove_absolute(path);
			} else if (rollback_files.has(path)) {
				Ref<FileAccess> restored = FileAccess::open(path, FileAccess::WRITE);
				if (restored.is_valid()) {
					restored->store_string(rollback_files[path]);
				}
			}
		}
		_append_message(TTR("Error"), failure + " " + TTR("Applied files were rolled back."));
	} else {
		_append_message(TTR("System"), vformat(TTR("Applied %d staged file(s). Use Undo Apply to restore them."), staged_files.size()));
		staged_files.clear();
		EditorFileSystem::get_singleton()->scan_changes();
	}
	_update_staging_controls();
}

void AIAssistantPlugin::_undo_last_apply() {
	for (const String &path : rollback_created_files) {
		DirAccess::remove_absolute(path);
	}
	for (const KeyValue<String, String> &entry : rollback_files) {
		Ref<FileAccess> restored = FileAccess::open(entry.key, FileAccess::WRITE);
		if (restored.is_valid()) {
			restored->store_string(entry.value);
		}
	}
	rollback_files.clear();
	rollback_created_files.clear();
	EditorFileSystem::get_singleton()->scan_changes();
	_append_message(TTR("System"), TTR("Restored files from before the last apply."));
	_update_staging_controls();
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
	tools.push_back(Dictionary{ { "type", "function" }, { "name", "write_file" }, { "description", "Stage a complete text file for user review. Read existing files before proposing replacements. Staged files are not written until the user approves them." }, { "parameters", write_parameters }, { "strict", true } });
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
		_start_codex(text);
		return;
	}

	Dictionary payload;
	payload["model"] = model->get_text().strip_edges();
	payload["instructions"] = "You are a coding agent inside the Godot editor. Inspect the project with list_files and read_file, then implement the user's request with write_file. Work only in res://. Preserve existing behavior, use idiomatic Godot 4 APIs, and finish with a concise summary of edits. Do not claim an edit unless the tool succeeded.";
	payload["input"] = text;
	payload["tools"] = _tool_definitions();
	_start_request(payload);
}

void AIAssistantPlugin::_start_codex(const String &p_prompt) {
	List<String> arguments;
	arguments.push_back("exec");
	arguments.push_back("--full-auto");
	arguments.push_back("--skip-git-repo-check");
	arguments.push_back("--color");
	arguments.push_back("never");
	arguments.push_back("-C");
	arguments.push_back(ProjectSettings::get_singleton()->globalize_path("res://"));
	const String selected_model = model->get_text().strip_edges();
	if (!selected_model.is_empty()) {
		arguments.push_back("--model");
		arguments.push_back(selected_model);
	}
	arguments.push_back(p_prompt);
	codex_output.clear();
	codex_process = OS::get_singleton()->execute_with_pipe(codex_path->get_text().strip_edges(), arguments, false);
	if (!codex_process.has("pid") || (int)codex_process["pid"] <= 0) {
		codex_process.clear();
		_append_message(TTR("Error"), TTR("Could not launch Codex CLI. Configure your PATH, install Codex, and run `codex login`."));
		_set_busy(false);
		return;
	}
	codex_started_at = OS::get_singleton()->get_ticks_msec();
}

void AIAssistantPlugin::_poll_codex() {
	const int64_t pid = codex_process["pid"];
	for (const char *stream_name : { "stdio", "stderr" }) {
		const String stream_key = stream_name;
		Ref<FileAccess> stream = codex_process.get(stream_key, Ref<FileAccess>());
		if (stream.is_valid() && stream->is_open()) {
			const uint64_t available = stream->get_length();
			if (available > 0) {
				PackedByteArray bytes;
				bytes.resize(MIN(available, uint64_t(64 * 1024)));
				const uint64_t read = stream->get_buffer(bytes.ptrw(), bytes.size());
				const String chunk = String::utf8(reinterpret_cast<const char *>(bytes.ptr()), read);
				codex_output += chunk;
				_append_message(stream_key == "stderr" ? TTR("Codex error") : TTR("Codex"), chunk.strip_edges());
			}
		}
	}
	if (OS::get_singleton()->get_ticks_msec() - codex_started_at > CODEX_TIMEOUT_MSEC) {
		OS::get_singleton()->kill(pid);
		_append_message(TTR("Error"), TTR("Codex was stopped after the 15 minute execution limit."));
		codex_process.clear();
		_set_busy(false);
		return;
	}
	if (!OS::get_singleton()->is_process_running(pid)) {
		const int exit_code = OS::get_singleton()->get_process_exit_code(pid);
		codex_process.clear();
		_finish_codex(exit_code);
	}
}

void AIAssistantPlugin::_finish_codex(int p_exit_code) {
	if (p_exit_code == 0) {
		EditorFileSystem::get_singleton()->scan_changes();
		_append_message(TTR("System"), TTR("Codex finished. Review all project changes in version control."));
	} else {
		_append_message(TTR("Error"), vformat(TTR("Codex exited with code %d."), p_exit_code));
	}
	_set_busy(false);
}

void AIAssistantPlugin::_stop() {
	if (!busy) {
		return;
	}
	if (!codex_process.is_empty()) {
		const int64_t pid = codex_process["pid"];
		if (OS::get_singleton()->is_process_running(pid)) {
			OS::get_singleton()->kill(pid);
		}
		codex_process.clear();
		_set_busy(false);
		_append_message(TTR("System"), TTR("Codex process stopped."));
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
	const String status = response.get("status", "");
	if (status == "failed" || status == "cancelled" || status == "incomplete") {
		_append_message(TTR("Error"), vformat(TTR("OpenAI response ended with status '%s'. No staged changes were applied."), status));
		_set_busy(false);
		return;
	}
	previous_response_id = response.get("id", "");
	Array outputs = response.get("output", Array());
	Array tool_outputs;
	String answer;
	for (const Variant &output_variant : outputs) {
		if (output_variant.get_type() != Variant::DICTIONARY) {
			continue;
		}
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
				if (content_variant.get_type() != Variant::DICTIONARY) {
					continue;
				}
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
	codex_path = memnew(LineEdit);
	codex_path->set_placeholder(TTRC("Path to Codex executable"));
	codex_path->set_text("codex");
	codex_path->set_tooltip_text(TTRC("Use an absolute path if Godot does not inherit your shell PATH."));
	root->add_child(codex_path);

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
	_load_history();

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

	HBoxContainer *review_actions = memnew(HBoxContainer);
	root->add_child(review_actions);
	apply_button = memnew(Button(TTRC("Apply Changes")));
	apply_button->set_disabled(true);
	apply_button->connect(SceneStringName(pressed), callable_mp(this, &AIAssistantPlugin::_apply_staged_files));
	review_actions->add_child(apply_button);
	discard_button = memnew(Button(TTRC("Discard")));
	discard_button->set_disabled(true);
	discard_button->connect(SceneStringName(pressed), callable_mp(this, &AIAssistantPlugin::_discard_staged_files));
	review_actions->add_child(discard_button);
	undo_button = memnew(Button(TTRC("Undo Apply")));
	undo_button->set_disabled(true);
	undo_button->connect(SceneStringName(pressed), callable_mp(this, &AIAssistantPlugin::_undo_last_apply));
	review_actions->add_child(undo_button);

	request = memnew(HTTPRequest);
	request->set_timeout(120);
	request->set_body_size_limit(8 * 1024 * 1024);
	request->connect("request_completed", callable_mp(this, &AIAssistantPlugin::_request_completed));
	add_child(request);
	set_process(true);

	EditorDockManager::get_singleton()->add_dock(dock);
}

AIAssistantPlugin::~AIAssistantPlugin() {
	if (!codex_process.is_empty()) {
		const int64_t pid = codex_process["pid"];
		if (OS::get_singleton()->is_process_running(pid)) {
			OS::get_singleton()->kill(pid);
		}
	}
	if (dock) {
		EditorDockManager::get_singleton()->remove_dock(dock);
		memdelete(dock);
	}
}
