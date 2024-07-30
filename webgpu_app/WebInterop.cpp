/*****************************************************************************
 * weBIGeo
 * Copyright (C) 2024 Gerald Kimmersdorfer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#include "WebInterop.h"
#include <array>

void global_canvas_size_changed(int width, int height) { WebInterop::_canvas_size_changed(width, height); }

void global_mouse_button_event(int button, int action, int mods, double xpos, double ypos)
{
    WebInterop::_mouse_button_event(button, action, mods, xpos, ypos);
}

void global_mouse_position_event(int button, double xpos, double ypos) { WebInterop::_mouse_position_event(button, xpos, ypos); }

void WebInterop::_canvas_size_changed(int width, int height) {
    emit instance().canvas_size_changed(width, height);
}

void WebInterop::_touch_event(const JsTouchEvent& event) {
    emit instance().touch_event(event);
}

void WebInterop::_mouse_button_event(int button, int action, int mods, double xpos, double ypos)
{
    emit instance().mouse_button_event(button, action, mods, xpos, ypos);
}

void WebInterop::_mouse_position_event([[maybe_unused]] int button, double xpos, double ypos) { emit instance().mouse_position_event(xpos, ypos); }

// Emscripten binding
EMSCRIPTEN_BINDINGS(webinterop_module) {
    emscripten::function("canvas_size_changed", &WebInterop::_canvas_size_changed);
    emscripten::function("touch_event", &WebInterop::_touch_event);
    emscripten::function("mouse_button_event", &WebInterop::_mouse_button_event);
    emscripten::function("mouse_position_event", &WebInterop::_mouse_position_event);

    emscripten::value_object<WebInterop::JsTouch>("JsTouch")
        .field("clientX", &WebInterop::JsTouch::clientX)
        .field("clientY", &WebInterop::JsTouch::clientY)
        .field("identifier", &WebInterop::JsTouch::identifier);

    emscripten::value_object<WebInterop::JsTouchEvent>("JsTouchEvent")
        .field("changedTouches", &WebInterop::JsTouchEvent::changedTouches)
        .field("touches", &WebInterop::JsTouchEvent::touches)
        .field("type", &WebInterop::JsTouchEvent::typeint);

    emscripten::value_array<std::array<WebInterop::JsTouch, JS_MAX_TOUCHES>>("arrayJsTouch")
        .element(emscripten::index<0>())
        .element(emscripten::index<1>())
        .element(emscripten::index<2>());
    // NOTE: add/remove elements if JS_MAX_TOUCHES is changed
}
