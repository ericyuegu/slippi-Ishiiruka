// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <array>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/MathUtil.h"
#include "Common/StringUtil.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/Pipes/Pipes.h"
#include "Core/ConfigManager.h"

namespace ciface
{
namespace Pipes
{
static const std::array<std::string, 12> s_button_tokens{
    {"A", "B", "X", "Y", "Z", "START", "L", "R", "D_UP", "D_DOWN", "D_LEFT", "D_RIGHT"}};

static const std::array<std::string, 2> s_shoulder_tokens{{"L", "R"}};

static const std::array<std::string, 2> s_axis_tokens{{"MAIN", "C"}};

static double StringToDouble(const std::string& text)
{
  std::istringstream is(text);
  // ignore current locale
  is.imbue(std::locale::classic());
  double result;
  is >> result;
  return result;
}

void PopulateDevices()
{
  #ifdef _WIN32
  PIPE_FD pipes[4];
  // Windows has named pipes, but they're different. They don't exist on the
  //  local filesystem and are transient. So rather than searching the /Pipes
  //  directory for pipes, we just always assume there's 4 and then make them
  for (uint32_t i = 0; i < 4; i++)
  {
    std::string pipename = "\\\\.\\pipe\\slippibot" + std::to_string(i+1);
    pipes[i] = CreateNamedPipeA(
       pipename.data(),              // pipe name
       PIPE_ACCESS_INBOUND,          // read access, inward only
       PIPE_TYPE_BYTE | PIPE_NOWAIT, // byte mode, nonblocking
       1,                            // number of clients
       256,                          // output buffer size
       256,                          // input buffer size
       0,                            // timeout value
       NULL                          // security attributes
    );

    // We're in nonblocking mode, so this won't wait for clients
    ConnectNamedPipe(pipes[i], NULL);
    std::string ui_pipe_name = "slippibot" + std::to_string(i+1);
    g_controller_interface.AddDevice(std::make_shared<PipeDevice>(pipes[i], ui_pipe_name));
  }
  #else

  // Search the Pipes directory for files that we can open in read-only,
  // non-blocking mode. The device name is the virtual name of the file.
  File::FSTEntry fst;
  std::string dir_path = File::GetUserPath(D_PIPES_IDX);
  if (!File::Exists(dir_path))
    return;
  fst = File::ScanDirectoryTree(dir_path, false);
  if (!fst.isDirectory)
    return;
  for (unsigned int i = 0; i < fst.size; ++i)
  {
    const File::FSTEntry& child = fst.children[i];
    if (child.isDirectory)
      continue;
    PIPE_FD fd = open(child.physicalName.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
      continue;
    g_controller_interface.AddDevice(std::make_shared<PipeDevice>(fd, child.virtualName));
  }
  #endif
}

PipeDevice::PipeDevice(PIPE_FD fd, const std::string& name) : m_fd(fd), m_name(name)
{
  for (const auto& tok : s_button_tokens)
  {
    PipeInput* btn = new PipeInput("Button " + tok);
    AddInput(btn);
    m_buttons[tok] = btn;
  }
  for (const auto& tok : s_shoulder_tokens)
  {
    AddAxis(tok, 0.0);
  }
  for (const auto& tok : s_axis_tokens)
  {
    AddAxis(tok + " X", 0.5);
    AddAxis(tok + " Y", 0.5);
  }
}

PipeDevice::~PipeDevice()
{
  #ifdef _WIN32
  CloseHandle(m_fd);
  #else
  close(m_fd);
  #endif
}

s32 PipeDevice::readFromPipe(PIPE_FD file_descriptor, char *in_buffer, size_t size)
{
  #ifdef _WIN32

  u32 bytes_available = 0;
  DWORD bytesread = 0;
  bool peek_success = PeekNamedPipe(
    file_descriptor,
    NULL,
    0,
    NULL,
    (LPDWORD)&bytes_available,
    NULL
  );

  if(!peek_success && (GetLastError() == ERROR_BROKEN_PIPE))
  {
    DisconnectNamedPipe(file_descriptor);
    ConnectNamedPipe(file_descriptor, NULL);
    return -1;
  }

  if(peek_success && (bytes_available > 0))
  {
    bool success = ReadFile(
      file_descriptor,    // pipe handle
      in_buffer,          // buffer to receive reply
      (DWORD)std::min(bytes_available, (u32)size),        // size of buffer
      &bytesread,         // number of bytes read
      NULL);              // not overlapped
    if(!success)
    {
        return -1;
    }
  }
  return (s32)bytesread;
  #else
  return read(file_descriptor, in_buffer, size);
  #endif
}

void PipeDevice::UpdateInput()
{
  bool finished = false;
  bool wait_for_inputs = SConfig::GetInstance().m_blockingPipes && g_needInputForFrame;
  #ifndef _WIN32
  if(wait_for_inputs)
  {
    fd_set set;
    FD_ZERO (&set);
    FD_SET (m_fd, &set);

    // Wait for activity on the socket
    select(m_fd+1, &set, NULL, NULL, NULL);
  }
  #endif
  do
  {
    // Read any pending characters off the pipe. If we hit a newline,
    // then dequeue a command off the front of m_buf and parse it.
    char buf[32];
    s32 bytes_read = readFromPipe(m_fd, buf, sizeof buf);
    if (bytes_read == 0) {
      // Pipe died, so just quit out
      return;
    }
    while (bytes_read > 0)
    {
      m_buf.append(buf, bytes_read);
      bytes_read = readFromPipe(m_fd, buf, sizeof buf);
    }
    std::size_t newline = m_buf.find("\n");
    while (newline != std::string::npos)
    {
      std::string command = m_buf.substr(0, newline);
      finished = ParseCommand(command);

      m_buf.erase(0, newline + 1);
      newline = m_buf.find("\n");
    }
  } while(!finished && wait_for_inputs);
}

void PipeDevice::AddAxis(const std::string& name, double value)
{
  // Dolphin uses separate axes for left/right, which complicates things.
  PipeInput* ax_hi = new PipeInput("Axis " + name + " +");
  ax_hi->SetState(value);
  PipeInput* ax_lo = new PipeInput("Axis " + name + " -");
  ax_lo->SetState(value);
  m_axes[name + " +"] = ax_hi;
  m_axes[name + " -"] = ax_lo;
  AddAnalogInputs(ax_lo, ax_hi);
}

u8 FloatToU8(double value)
{
  // Match the empirical behavior of the named pipes.
  s8 raw = std::floor((value - 0.5) * 254);
  return reinterpret_cast<u8 &>(raw);
}

void PipeDevice::SetAxis(const std::string& entry, double value)
{
  value = MathUtil::Clamp(value, 0.0, 1.0);

  if (entry.compare("MAIN X") == 0)
  {
    m_current_pad.padBuf[2] = FloatToU8(value);
  }
  if (entry.compare("MAIN Y") == 0)
  {
    m_current_pad.padBuf[3] = FloatToU8(value);
  }
  if (entry.compare("C X") == 0)
  {
    m_current_pad.padBuf[4] = FloatToU8(value);
  }
  if (entry.compare("C Y") == 0)
  {
    m_current_pad.padBuf[5] = FloatToU8(value);
  }
  if (entry.compare("L") == 0)
  {
    m_current_pad.padBuf[6] = u8 (value * 255);
  }
  if (entry.compare("R") == 0)
  {
    m_current_pad.padBuf[7] = u8 (value * 255);
  }

  double hi = std::max(0.0, value - 0.5) * 2.0;
  double lo = (0.5 - std::min(0.5, value)) * 2.0;
  auto search_hi = m_axes.find(entry + " +");
  if (search_hi != m_axes.end())
    search_hi->second->SetState(hi);
  auto search_lo = m_axes.find(entry + " -");
  if (search_lo != m_axes.end())
    search_lo->second->SetState(lo);
}

bool PipeDevice::ParseCommand(const std::string& command)
{
  if(command == "FLUSH")
  {
    // Let ControllerInterface.cpp clear the flag after all PipeDevices
    // have been queried.
    // g_needInputForFrame = false;
    return true;
  }
  std::vector<std::string> tokens;
  SplitString(command, ' ', tokens);
  if (tokens.size() < 2 || tokens.size() > 4)
    return false;
  if (tokens[0] == "PRESS" || tokens[0] == "RELEASE")
  {
    SetButtonState(tokens[1], tokens[0]);
    auto search = m_buttons.find(tokens[1]);
    if (search != m_buttons.end())
      search->second->SetState(tokens[0] == "PRESS" ? 1.0 : 0.0);
  }
  else if (tokens[0] == "SET")
  {
    if (tokens.size() == 3)
    {
      double value = StringToDouble(tokens[2]);
      SetAxis(tokens[1], value);
    }
    else if (tokens.size() == 4)
    {
      double x = StringToDouble(tokens[2]);
      double y = StringToDouble(tokens[3]);
      SetAxis(tokens[1] + " X", x);
      SetAxis(tokens[1] + " Y", y);
    }
  }
  return false;
}

SlippiPad PipeDevice::GetSlippiPad()
{
  return m_current_pad;
}

void PipeDevice::SetButtonState(const std::string& button, const std::string& press)
{
  u8 mask = 0x00;
  int index = 0;
  bool is_press = press == "PRESS";

  if (button.compare("A") == 0)
  {
    mask = 0x01;
    index = 0;
  }
  if (button.compare("B") == 0)
  {
    mask = 0x02;
    index = 0;
  }
  if (button.compare("X") == 0)
  {
    mask = 0x04;
    index = 0;
  }
  if (button.compare("Y") == 0)
  {
    mask = 0x08;
    index = 0;
  }
  if (button.compare("L") == 0)
  {
    mask = 0x40;
    index = 1;
  }
  if (button.compare("R") == 0)
  {
    mask = 0x20;
    index = 1;
  }
  if (button.compare("START") == 0)
  {
    mask = 0x10;
    index = 0;
  }
  if (button.compare("D_LEFT") == 0)
  {
    mask = 0x01;
    index = 1;
  }
  if (button.compare("D_RIGHT") == 0)
  {
    mask = 0x02;
    index = 1;
  }
  if (button.compare("D_DOWN") == 0)
  {
    mask = 0x04;
    index = 1;
  }
  if (button.compare("D_UP") == 0)
  {
    mask = 0x08;
    index = 1;
  }
  if (button.compare("Z") == 0)
  {
    mask = 0x10;
    index = 1;
  }
  if (is_press)
  {
    m_current_pad.padBuf[index] |= mask;
  }
  else
  {
    m_current_pad.padBuf[index] &= ~(mask);
  }
}

}
}
