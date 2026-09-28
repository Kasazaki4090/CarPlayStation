# Win32 UI

This directory contains the initial Windows UI implementation.

It provides the basic application interface for CarPlayStation using Win32 APIs.

Expected contents include:

* Application window
* Window lifecycle
* Display surface
* Basic input collection
* Basic application controls

The Win32 UI must communicate through the application layer.

It must not directly access:

* CarPlay protocol internals
* Transport implementations
* Media decoder implementations
* USB/NCM internals
* Third-party libraries

The UI is replaceable. Future UI implementations may use other technologies without requiring changes to the underlying CarPlay Core.
