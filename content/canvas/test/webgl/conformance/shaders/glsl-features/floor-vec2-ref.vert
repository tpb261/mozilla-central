// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

attribute vec4 aPosition;
attribute vec2 aTexcoord;

varying vec2 vTexcoord;
varying vec4 vColor;

float floor_emu1(float value) {
  return value - mod(value, 1.0);
}

vec2 floor_emu(vec2 value) {
  return vec2(
	  floor_emu1(value.x),
	  floor_emu1(value.y));
}

void main()
{
   gl_Position = aPosition;
   vTexcoord = aTexcoord;
   vec4 color = vec4(
       aTexcoord,
       aTexcoord.x * aTexcoord.y,
       (1.0 - aTexcoord.x) * aTexcoord.y * 0.5 + 0.5);
   vColor = vec4(
       floor(color.xy * 8.0 - vec2(4, 4)) / 8.0 + vec2(0.5, 0.5),
       0,
       1);
}




