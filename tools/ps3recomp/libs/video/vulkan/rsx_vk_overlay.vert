#version 450
// ps3recomp Vulkan backend: host overlay (on-screen touch controls).
// Position is already in Vulkan NDC (+y down); colour is straight alpha.
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec4 in_col;
layout(location = 0) out vec4 v_col;
void main()
{
    gl_Position = vec4(in_pos, 0.0, 1.0);
    v_col = in_col;
}
