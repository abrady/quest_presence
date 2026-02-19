/*
 * XrPresenceTest - Complete OpenXR + ImGui VR App for Quest 3 Presence Testing
 *
 * This app allows testing the Group Presence / Invite Panel flow with
 * buttons rendered in VR space using Dear ImGui and OpenXR compositor layers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <android_native_app_glue.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#define XR_USE_PLATFORM_ANDROID 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// Dear ImGui
#define IMGUI_IMPL_OPENGL_ES3
#include "imgui/imgui.h"
#include "imgui/imgui_impl_opengl3.h"

#define TAG "XrPresenceTest"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

#define MATH_PI 3.14159265358979323846f

// App configuration
static const char* DESTINATION_API_NAME = "test-location";
static const char* APP_ID = "33969008956076849";

// UI Panel dimensions
static const int UI_WIDTH = 1024;
static const int UI_HEIGHT = 768;

// Forward declaration for error checking
static XrInstance g_Instance = XR_NULL_HANDLE;
XrInstance GetXrInstance() { return g_Instance; }

// OpenXR error checking macro
#define OXR(func) do { \
    XrResult _result = func; \
    if (XR_FAILED(_result)) { \
        char errorBuffer[XR_MAX_RESULT_STRING_SIZE]; \
        xrResultToString(g_Instance, _result, errorBuffer); \
        ALOGE("OpenXR error: %s: %s", #func, errorBuffer); \
    } \
} while(0)

// ================================================================================
// Swapchain structure
// ================================================================================
typedef struct {
    XrSwapchain Handle;
    uint32_t Width;
    uint32_t Height;
    uint32_t ImageCount;
    GLuint* ColorTextures;
} ovrSwapChain;

// ================================================================================
// EGL Context
// ================================================================================
typedef struct {
    EGLDisplay Display;
    EGLConfig Config;
    EGLContext Context;
    EGLSurface TinySurface;
} ovrEgl;

static void ovrEgl_CreateContext(ovrEgl* egl) {
    egl->Display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(egl->Display, NULL, NULL);

    const EGLint configAttribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_SAMPLES, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_NONE
    };

    EGLint numConfigs;
    eglChooseConfig(egl->Display, configAttribs, &egl->Config, 1, &numConfigs);

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    egl->Context = eglCreateContext(egl->Display, egl->Config, EGL_NO_CONTEXT, contextAttribs);

    const EGLint surfaceAttribs[] = {
        EGL_WIDTH, 16,
        EGL_HEIGHT, 16,
        EGL_NONE
    };
    egl->TinySurface = eglCreatePbufferSurface(egl->Display, egl->Config, surfaceAttribs);

    eglMakeCurrent(egl->Display, egl->TinySurface, egl->TinySurface, egl->Context);

    ALOGI("EGL context created");
}

static void ovrEgl_DestroyContext(ovrEgl* egl) {
    if (egl->Display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl->Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl->Context != EGL_NO_CONTEXT) {
            eglDestroyContext(egl->Display, egl->Context);
        }
        if (egl->TinySurface != EGL_NO_SURFACE) {
            eglDestroySurface(egl->Display, egl->TinySurface);
        }
        eglTerminate(egl->Display);
    }
    memset(egl, 0, sizeof(ovrEgl));
}

// ================================================================================
// Swapchain functions
// ================================================================================
static void ovrSwapChain_Create(XrSession session, ovrSwapChain* sc, int width, int height) {
    XrSwapchainCreateInfo swapchainCreateInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainCreateInfo.createFlags = 0;
    swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainCreateInfo.format = GL_SRGB8_ALPHA8;
    swapchainCreateInfo.sampleCount = 1;
    swapchainCreateInfo.width = width;
    swapchainCreateInfo.height = height;
    swapchainCreateInfo.faceCount = 1;
    swapchainCreateInfo.arraySize = 1;
    swapchainCreateInfo.mipCount = 1;

    OXR(xrCreateSwapchain(session, &swapchainCreateInfo, &sc->Handle));
    sc->Width = width;
    sc->Height = height;

    OXR(xrEnumerateSwapchainImages(sc->Handle, 0, &sc->ImageCount, NULL));

    XrSwapchainImageOpenGLESKHR* images = (XrSwapchainImageOpenGLESKHR*)malloc(
        sc->ImageCount * sizeof(XrSwapchainImageOpenGLESKHR));
    for (uint32_t i = 0; i < sc->ImageCount; i++) {
        images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        images[i].next = NULL;
    }

    OXR(xrEnumerateSwapchainImages(sc->Handle, sc->ImageCount, &sc->ImageCount,
        (XrSwapchainImageBaseHeader*)images));

    sc->ColorTextures = (GLuint*)malloc(sc->ImageCount * sizeof(GLuint));
    for (uint32_t i = 0; i < sc->ImageCount; i++) {
        sc->ColorTextures[i] = images[i].image;
    }

    free(images);
    ALOGI("Swapchain created: %dx%d, %u images", width, height, sc->ImageCount);
}

static void ovrSwapChain_Destroy(ovrSwapChain* sc) {
    if (sc->Handle != XR_NULL_HANDLE) {
        OXR(xrDestroySwapchain(sc->Handle));
    }
    free(sc->ColorTextures);
    memset(sc, 0, sizeof(ovrSwapChain));
}

// ================================================================================
// Application State
// ================================================================================
typedef struct {
    struct android_app* NativeApp;

    ovrEgl Egl;
    XrSystemId SystemId;
    XrSession Session;
    XrSpace LocalSpace;
    XrSpace HeadSpace;

    ovrSwapChain UiSwapChain;
    GLuint UiFramebuffer;

    bool Resumed;
    bool SessionActive;
    bool Running;

    // Presence state
    bool PresenceSet;
    bool IsJoinable;
    char LobbyId[64];
    char StatusText[256];
    char LogBuffer[8192];

    // Input state
    XrActionSet ActionSet;
    XrAction TriggerAction;
    XrAction AimPoseAction;
    XrSpace LeftAimSpace;
    XrSpace RightAimSpace;
    XrPath LeftHandPath;
    XrPath RightHandPath;

    // Cursor state
    float CursorX;
    float CursorY;
    bool TriggerPressed;
    bool TriggerJustPressed;
    int SelectedButton;
} ovrApp;

static ovrApp appState;

// ================================================================================
// Logging
// ================================================================================
static void AppendLog(const char* fmt, ...) {
    char temp[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(temp, sizeof(temp), fmt, args);
    va_end(args);

    size_t logLen = strlen(appState.LogBuffer);
    size_t tempLen = strlen(temp);
    if (logLen + tempLen + 2 > sizeof(appState.LogBuffer) - 100) {
        memmove(appState.LogBuffer, appState.LogBuffer + 1024, logLen - 1024 + 1);
        logLen = strlen(appState.LogBuffer);
    }
    strcat(appState.LogBuffer, temp);
    strcat(appState.LogBuffer, "\n");

    ALOGI("%s", temp);
}

// ================================================================================
// Presence Functions (Mocked - replace with real Oculus Platform SDK)
// ================================================================================
static void GenerateLobbyId() {
    snprintf(appState.LobbyId, sizeof(appState.LobbyId), "lobby_%d_%ld",
             rand() % 10000, (long)time(NULL));
    AppendLog("Generated lobby ID: %s", appState.LobbyId);
}

static void SetGroupPresence() {
    if (strlen(appState.LobbyId) == 0) {
        GenerateLobbyId();
    }

    AppendLog("Setting group presence...");
    AppendLog("  Destination: %s", DESTINATION_API_NAME);
    AppendLog("  LobbyId: %s", appState.LobbyId);
    AppendLog("  IsJoinable: true");

    // TODO: Real SDK call
    // ovr_GroupPresence_Set(options);

    appState.PresenceSet = true;
    appState.IsJoinable = true;
    snprintf(appState.StatusText, sizeof(appState.StatusText),
             "Presence SET - Ready to invite!");
    AppendLog("Presence set successfully (MOCKED)");
}

static void ClearGroupPresence() {
    AppendLog("Clearing group presence...");

    // TODO: Real SDK call
    // ovr_GroupPresence_Clear();

    appState.PresenceSet = false;
    appState.IsJoinable = false;
    appState.LobbyId[0] = '\0';
    snprintf(appState.StatusText, sizeof(appState.StatusText), "Presence cleared");
    AppendLog("Presence cleared (MOCKED)");
}

static void LaunchInvitePanel() {
    if (!appState.PresenceSet || !appState.IsJoinable) {
        AppendLog("!! WARNING: Launching invite panel but:");
        if (!appState.PresenceSet) AppendLog("   - Presence NOT set!");
        if (!appState.IsJoinable) AppendLog("   - User NOT joinable!");
        AppendLog("   This will cause panel to close immediately!");
    }

    AppendLog("Launching invite panel...");

    // TODO: Real SDK call
    // ovr_GroupPresence_LaunchInvitePanel(options);

    snprintf(appState.StatusText, sizeof(appState.StatusText),
             "Invite panel launched (MOCKED)");
    AppendLog("In real implementation, system panel would appear");
}

static void TestBuggyFlow() {
    AppendLog("=== BUGGY FLOW (Developer's Issue) ===");
    AppendLog("Order: Panel -> Presence (WRONG!)");
    AppendLog("Result: Panel closes immediately");

    LaunchInvitePanel();  // Called too early!
    SetGroupPresence();   // Too late
}

static void TestCorrectFlow() {
    AppendLog("=== CORRECT FLOW ===");
    AppendLog("Order: Lobby -> Presence -> Panel");

    GenerateLobbyId();
    SetGroupPresence();
    AppendLog("Now safe to open invite panel!");
}

// ================================================================================
// ImGui Rendering
// ================================================================================
static bool g_ImGuiInitialized = false;

static void InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.DisplaySize = ImVec2((float)UI_WIDTH, (float)UI_HEIGHT);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.FontGlobalScale = 2.5f;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 12.0f;
    style.FrameRounding = 8.0f;
    style.ItemSpacing = ImVec2(16, 12);
    style.FramePadding = ImVec2(12, 8);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.95f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.2f, 0.4f, 0.8f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.3f, 0.5f, 0.9f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.3f, 0.6f, 1.0f);

    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_ImGuiInitialized = true;

    ALOGI("ImGui initialized");
}

static void ShutdownImGui() {
    if (g_ImGuiInitialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        g_ImGuiInitialized = false;
    }
}

static void RenderImGuiToTexture(GLuint targetTexture) {
    if (!g_ImGuiInitialized) return;

    // Bind framebuffer with target texture
    glBindFramebuffer(GL_FRAMEBUFFER, appState.UiFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targetTexture, 0);

    glViewport(0, 0, UI_WIDTH, UI_HEIGHT);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Update ImGui input
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = ImVec2(appState.CursorX, appState.CursorY);
    io.MouseDown[0] = appState.TriggerPressed;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    // Main window
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(UI_WIDTH - 40, UI_HEIGHT - 40), ImGuiCond_Always);

    ImGui::Begin("Quest 3 Presence Test", NULL,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    // Title
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.8f, 1.0f, 1.0f));
    ImGui::Text("Group Presence / Invite Panel Test");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Status
    if (appState.PresenceSet && appState.IsJoinable) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
    }
    ImGui::Text("Status: %s", appState.StatusText);
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Text("Presence Set: %s", appState.PresenceSet ? "YES" : "NO");
    ImGui::Text("Is Joinable: %s", appState.IsJoinable ? "YES" : "NO");
    if (strlen(appState.LobbyId) > 0) {
        ImGui::Text("Lobby: %s", appState.LobbyId);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Action buttons - two columns
    ImGui::Text("Individual Actions:");
    ImGui::Spacing();

    float buttonWidth = 280.0f;
    float buttonHeight = 60.0f;

    if (ImGui::Button("Generate Lobby", ImVec2(buttonWidth, buttonHeight))) {
        GenerateLobbyId();
    }
    ImGui::SameLine();
    if (ImGui::Button("Set Presence", ImVec2(buttonWidth, buttonHeight))) {
        SetGroupPresence();
    }

    if (ImGui::Button("Clear Presence", ImVec2(buttonWidth, buttonHeight))) {
        ClearGroupPresence();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Invite Panel", ImVec2(buttonWidth, buttonHeight))) {
        LaunchInvitePanel();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Test Flows:");
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
    if (ImGui::Button("CORRECT Flow", ImVec2(buttonWidth, buttonHeight))) {
        TestCorrectFlow();
    }
    ImGui::PopStyleColor();

    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.1f, 1.0f));
    if (ImGui::Button("BUGGY Flow", ImVec2(buttonWidth, buttonHeight))) {
        TestBuggyFlow();
    }
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Log window
    ImGui::Text("Log:");
    ImGui::BeginChild("LogRegion", ImVec2(0, 180), true);
    ImGui::TextUnformatted(appState.LogBuffer);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    if (ImGui::Button("Clear Log", ImVec2(140, 40))) {
        appState.LogBuffer[0] = '\0';
    }

    ImGui::End();

    // Draw cursor crosshair so user can see where they're pointing
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    float cx = appState.CursorX;
    float cy = appState.CursorY;
    ImU32 cursorColor = appState.TriggerPressed ?
        IM_COL32(255, 100, 100, 255) : IM_COL32(100, 255, 100, 255);

    // Draw crosshair
    drawList->AddLine(ImVec2(cx - 20, cy), ImVec2(cx + 20, cy), cursorColor, 3.0f);
    drawList->AddLine(ImVec2(cx, cy - 20), ImVec2(cx, cy + 20), cursorColor, 3.0f);
    drawList->AddCircle(ImVec2(cx, cy), 15.0f, cursorColor, 16, 3.0f);

    // Show cursor position for debugging
    char cursorText[64];
    snprintf(cursorText, sizeof(cursorText), "Cursor: %.0f, %.0f", cx, cy);
    drawList->AddText(ImVec2(10, UI_HEIGHT - 30), IM_COL32(255, 255, 255, 200), cursorText);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ================================================================================
// Input Handling
// ================================================================================
static void SetupInput() {
    // Create action set
    XrActionSetCreateInfo actionSetInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(actionSetInfo.actionSetName, "gameplay");
    strcpy(actionSetInfo.localizedActionSetName, "Gameplay");
    actionSetInfo.priority = 0;
    OXR(xrCreateActionSet(g_Instance, &actionSetInfo, &appState.ActionSet));

    // Create paths
    OXR(xrStringToPath(g_Instance, "/user/hand/left", &appState.LeftHandPath));
    OXR(xrStringToPath(g_Instance, "/user/hand/right", &appState.RightHandPath));
    XrPath handPaths[2] = {appState.LeftHandPath, appState.RightHandPath};

    // Trigger action
    XrActionCreateInfo actionInfo = {XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    strcpy(actionInfo.actionName, "trigger");
    strcpy(actionInfo.localizedActionName, "Trigger");
    actionInfo.countSubactionPaths = 2;
    actionInfo.subactionPaths = handPaths;
    OXR(xrCreateAction(appState.ActionSet, &actionInfo, &appState.TriggerAction));

    // Aim pose action
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy(actionInfo.actionName, "aim_pose");
    strcpy(actionInfo.localizedActionName, "Aim Pose");
    OXR(xrCreateAction(appState.ActionSet, &actionInfo, &appState.AimPoseAction));

    // Suggest bindings for Touch controllers
    XrPath touchProfilePath;
    OXR(xrStringToPath(g_Instance, "/interaction_profiles/oculus/touch_controller", &touchProfilePath));

    XrPath triggerLeftPath, triggerRightPath, aimLeftPath, aimRightPath;
    OXR(xrStringToPath(g_Instance, "/user/hand/left/input/trigger/value", &triggerLeftPath));
    OXR(xrStringToPath(g_Instance, "/user/hand/right/input/trigger/value", &triggerRightPath));
    OXR(xrStringToPath(g_Instance, "/user/hand/left/input/aim/pose", &aimLeftPath));
    OXR(xrStringToPath(g_Instance, "/user/hand/right/input/aim/pose", &aimRightPath));

    XrActionSuggestedBinding bindings[] = {
        {appState.TriggerAction, triggerLeftPath},
        {appState.TriggerAction, triggerRightPath},
        {appState.AimPoseAction, aimLeftPath},
        {appState.AimPoseAction, aimRightPath},
    };

    XrInteractionProfileSuggestedBinding suggestedBindings = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBindings.interactionProfile = touchProfilePath;
    suggestedBindings.suggestedBindings = bindings;
    suggestedBindings.countSuggestedBindings = 4;
    OXR(xrSuggestInteractionProfileBindings(g_Instance, &suggestedBindings));

    ALOGI("Input actions created");
}

static void AttachActionSet() {
    XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &appState.ActionSet;
    OXR(xrAttachSessionActionSets(appState.Session, &attachInfo));

    // Create action spaces
    XrActionSpaceCreateInfo spaceInfo = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action = appState.AimPoseAction;
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;

    spaceInfo.subactionPath = appState.LeftHandPath;
    OXR(xrCreateActionSpace(appState.Session, &spaceInfo, &appState.LeftAimSpace));

    spaceInfo.subactionPath = appState.RightHandPath;
    OXR(xrCreateActionSpace(appState.Session, &spaceInfo, &appState.RightAimSpace));

    ALOGI("Action set attached");
}

static void UpdateInput(XrTime predictedTime) {
    // Sync actions
    XrActiveActionSet activeActionSet = {appState.ActionSet, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    OXR(xrSyncActions(appState.Session, &syncInfo));

    // Get trigger state (use right hand)
    XrActionStateGetInfo getInfo = {XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = appState.TriggerAction;
    getInfo.subactionPath = appState.RightHandPath;

    XrActionStateFloat triggerState = {XR_TYPE_ACTION_STATE_FLOAT};
    OXR(xrGetActionStateFloat(appState.Session, &getInfo, &triggerState));

    bool wasPressed = appState.TriggerPressed;
    appState.TriggerPressed = triggerState.currentState > 0.5f;
    appState.TriggerJustPressed = appState.TriggerPressed && !wasPressed;

    // Get aim pose (right hand)
    XrSpaceLocation aimLoc = {XR_TYPE_SPACE_LOCATION};
    OXR(xrLocateSpace(appState.RightAimSpace, appState.LocalSpace, predictedTime, &aimLoc));

    if (aimLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
        float px = aimLoc.pose.position.x;
        float py = aimLoc.pose.position.y;
        float pz = aimLoc.pose.position.z;

        // Get forward direction from quaternion (Z-forward in OpenXR)
        XrQuaternionf q = aimLoc.pose.orientation;

        // Forward vector (negative Z in local space, transformed by quaternion)
        // For a quaternion q, rotating vector v: v' = q * v * q^-1
        // Simplified for unit forward vector (0, 0, -1):
        float dx = -2.0f * (q.x * q.z - q.w * q.y);
        float dy = -2.0f * (q.y * q.z + q.w * q.x);
        float dz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));

        // Panel is at z = -2.0 in local space
        float panelZ = -2.0f;

        // Check if ray is pointing towards the panel (dz should be negative)
        if (dz < -0.001f) {
            float t = (panelZ - pz) / dz;
            if (t > 0 && t < 100.0f) {  // Valid intersection in front of controller
                float hitX = px + dx * t;
                float hitY = py + dy * t;

                // Panel dimensions in world space (1.6m wide, 1.2m tall, centered at origin)
                float panelWidth = 1.6f;
                float panelHeight = 1.2f;

                // Convert world hit position to UI coordinates
                // Panel X: -0.8 to 0.8 maps to UI 1024 to 0 (inverted)
                // Panel Y: -0.6 to 0.6 maps to UI 0 to 768 (inverted)
                appState.CursorX = (0.5f - (hitX / panelWidth)) * UI_WIDTH;
                appState.CursorY = ((hitY / panelHeight) + 0.5f) * UI_HEIGHT;

                // Clamp to UI bounds
                if (appState.CursorX < 0) appState.CursorX = 0;
                if (appState.CursorX > UI_WIDTH) appState.CursorX = UI_WIDTH;
                if (appState.CursorY < 0) appState.CursorY = 0;
                if (appState.CursorY > UI_HEIGHT) appState.CursorY = UI_HEIGHT;
            }
        }
    }
}

// ================================================================================
// Session Management
// ================================================================================
static void HandleSessionStateChange(XrSessionState state) {
    ALOGI("Session state: %d", state);

    switch (state) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            OXR(xrBeginSession(appState.Session, &beginInfo));
            appState.SessionActive = true;
            AppendLog("VR Session started!");
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            OXR(xrEndSession(appState.Session));
            appState.SessionActive = false;
            AppendLog("VR Session stopped");
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            appState.Running = false;
            break;
        default:
            break;
    }
}

static void HandleAppCmd(struct android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_RESUME:
            appState.Resumed = true;
            ALOGI("App resumed");
            break;
        case APP_CMD_PAUSE:
            appState.Resumed = false;
            ALOGI("App paused");
            break;
        case APP_CMD_DESTROY:
            appState.Running = false;
            ALOGI("App destroyed");
            break;
    }
}

// ================================================================================
// Main Entry Point
// ================================================================================
void android_main(struct android_app* app) {
    ALOGI("XrPresenceTest starting...");

    JNIEnv* env;
    app->activity->vm->AttachCurrentThread(&env, NULL);
    prctl(PR_SET_NAME, (long)"XrPresence", 0, 0, 0);

    memset(&appState, 0, sizeof(appState));
    appState.NativeApp = app;
    appState.Running = true;
    strcpy(appState.StatusText, "Ready - Set presence before inviting!");

    srand((unsigned int)time(NULL));

    // Initialize loader
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          (PFN_xrVoidFunction*)&xrInitializeLoaderKHR);
    if (xrInitializeLoaderKHR) {
        XrLoaderInitInfoAndroidKHR loaderInfo = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loaderInfo.applicationVM = app->activity->vm;
        loaderInfo.applicationContext = app->activity->clazz;
        xrInitializeLoaderKHR((XrLoaderInitInfoBaseHeaderKHR*)&loaderInfo);
    }

    // Create OpenXR instance
    const char* extensions[] = {
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    };

    XrInstanceCreateInfoAndroidKHR androidInfo = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = app->activity->vm;
    androidInfo.applicationActivity = app->activity->clazz;

    XrInstanceCreateInfo instanceInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.next = &androidInfo;
    strcpy(instanceInfo.applicationInfo.applicationName, "XrPresenceTest");
    instanceInfo.applicationInfo.applicationVersion = 1;
    strcpy(instanceInfo.applicationInfo.engineName, "Custom");
    instanceInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.enabledExtensionNames = extensions;

    OXR(xrCreateInstance(&instanceInfo, &g_Instance));
    ALOGI("OpenXR instance created");

    // Get system
    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    OXR(xrGetSystem(g_Instance, &systemInfo, &appState.SystemId));
    ALOGI("System ID: %lu", (unsigned long)appState.SystemId);

    // Initialize EGL
    ovrEgl_CreateContext(&appState.Egl);

    // Check graphics requirements
    PFN_xrGetOpenGLESGraphicsRequirementsKHR xrGetOpenGLESGraphicsRequirementsKHR = NULL;
    xrGetInstanceProcAddr(g_Instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          (PFN_xrVoidFunction*)&xrGetOpenGLESGraphicsRequirementsKHR);
    XrGraphicsRequirementsOpenGLESKHR graphicsReqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    xrGetOpenGLESGraphicsRequirementsKHR(g_Instance, appState.SystemId, &graphicsReqs);

    // Create session
    XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    graphicsBinding.display = appState.Egl.Display;
    graphicsBinding.config = appState.Egl.Config;
    graphicsBinding.context = appState.Egl.Context;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &graphicsBinding;
    sessionInfo.systemId = appState.SystemId;
    OXR(xrCreateSession(g_Instance, &sessionInfo, &appState.Session));
    ALOGI("Session created");

    // Create reference spaces
    XrReferenceSpaceCreateInfo spaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    OXR(xrCreateReferenceSpace(appState.Session, &spaceInfo, &appState.LocalSpace));

    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    OXR(xrCreateReferenceSpace(appState.Session, &spaceInfo, &appState.HeadSpace));

    // Create UI swapchain
    ovrSwapChain_Create(appState.Session, &appState.UiSwapChain, UI_WIDTH, UI_HEIGHT);

    // Create framebuffer for UI rendering
    glGenFramebuffers(1, &appState.UiFramebuffer);

    // Setup input
    SetupInput();

    // Initialize ImGui
    InitImGui();

    // Attach actions after session is ready
    bool actionsAttached = false;

    app->userData = &appState;
    app->onAppCmd = HandleAppCmd;

    AppendLog("XrPresenceTest initialized!");
    AppendLog("App ID: %s", APP_ID);
    AppendLog("Destination: %s", DESTINATION_API_NAME);
    AppendLog("");
    AppendLog("Point controller at buttons");
    AppendLog("Pull trigger to click");

    // Main loop
    while (appState.Running) {
        // Process Android events
        int events;
        struct android_poll_source* source;
        int timeout = (!appState.Resumed && !appState.SessionActive) ? -1 : 0;

        while (ALooper_pollOnce(timeout, NULL, &events, (void**)&source) >= 0) {
            if (source) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                appState.Running = false;
                break;
            }
            timeout = 0;
        }

        // Process OpenXR events
        XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_Instance, &event) == XR_SUCCESS) {
            switch (event.type) {
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    XrEventDataSessionStateChanged* stateEvent =
                        (XrEventDataSessionStateChanged*)&event;
                    HandleSessionStateChange(stateEvent->state);

                    if (stateEvent->state == XR_SESSION_STATE_READY && !actionsAttached) {
                        AttachActionSet();
                        actionsAttached = true;
                    }
                    break;
                }
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    appState.Running = false;
                    break;
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }

        if (!appState.SessionActive) {
            continue;
        }

        // Wait for frame
        XrFrameState frameState = {XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
        OXR(xrWaitFrame(appState.Session, &waitInfo, &frameState));

        // Begin frame
        XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
        OXR(xrBeginFrame(appState.Session, &beginInfo));

        // Update input
        UpdateInput(frameState.predictedDisplayTime);

        // Acquire swapchain image
        uint32_t imageIndex;
        XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        OXR(xrAcquireSwapchainImage(appState.UiSwapChain.Handle, &acquireInfo, &imageIndex));

        XrSwapchainImageWaitInfo waitImageInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitImageInfo.timeout = XR_INFINITE_DURATION;
        OXR(xrWaitSwapchainImage(appState.UiSwapChain.Handle, &waitImageInfo));

        // Render ImGui to swapchain texture
        RenderImGuiToTexture(appState.UiSwapChain.ColorTextures[imageIndex]);

        // Release swapchain image
        XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        OXR(xrReleaseSwapchainImage(appState.UiSwapChain.Handle, &releaseInfo));

        // Build quad layer for UI
        XrCompositionLayerQuad quadLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quadLayer.space = appState.LocalSpace;
        quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quadLayer.subImage.swapchain = appState.UiSwapChain.Handle;
        quadLayer.subImage.imageRect.offset = {0, 0};
        quadLayer.subImage.imageRect.extent = {(int)UI_WIDTH, (int)UI_HEIGHT};
        quadLayer.subImage.imageArrayIndex = 0;

        // Position the quad in front of user
        quadLayer.pose.orientation.w = 1.0f;
        quadLayer.pose.position.x = 0.0f;
        quadLayer.pose.position.y = 0.0f;
        quadLayer.pose.position.z = -2.0f;
        quadLayer.size.width = 1.6f;
        quadLayer.size.height = 1.2f;

        // End frame
        const XrCompositionLayerBaseHeader* layers[] = {
            (XrCompositionLayerBaseHeader*)&quadLayer
        };

        XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = frameState.shouldRender ? 1 : 0;
        endInfo.layers = layers;

        OXR(xrEndFrame(appState.Session, &endInfo));
    }

    // Cleanup
    ShutdownImGui();

    if (appState.UiFramebuffer) {
        glDeleteFramebuffers(1, &appState.UiFramebuffer);
    }

    ovrSwapChain_Destroy(&appState.UiSwapChain);

    if (appState.LeftAimSpace) xrDestroySpace(appState.LeftAimSpace);
    if (appState.RightAimSpace) xrDestroySpace(appState.RightAimSpace);
    if (appState.LocalSpace) xrDestroySpace(appState.LocalSpace);
    if (appState.HeadSpace) xrDestroySpace(appState.HeadSpace);
    if (appState.Session) xrDestroySession(appState.Session);
    if (g_Instance) xrDestroyInstance(g_Instance);

    ovrEgl_DestroyContext(&appState.Egl);

    app->activity->vm->DetachCurrentThread();

    ALOGI("XrPresenceTest shutdown complete");
}
