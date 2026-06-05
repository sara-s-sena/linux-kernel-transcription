# Linux Kernel Transcription

Manual transcription of the Linux kernel source tree into a local development environment (VS Code + WSL2/Ubuntu), reproducing source files line-by-line from upstream kernel sources.

## Current progress
**~3,300 lines** transcribed across `block/blk-core.c` (completed) and the top-level `Makefile` (in progress).

## Methodology
Source code is entered manually without copy-paste to improve familiarity with large-scale C codebases and develop pattern recognition for kernel architecture, coding conventions, build systems, memory management, and low-level systems programming concepts.

## Context
Approximately 90 days into programming. Modern AI tools collapse barriers to information, expose new programmers to an unprecedented volume of knowledge, and continuously generate increasingly high-level abstractions. In that context, I found myself returning to something much older and harder to automate: direct contact with the source itself.
I do not know what the outcome of this experiment will be. What I do know is that we need more people building decades-deep familiarity with the infrastructure that underpins much of the world's digital systems. This project is my small attempt in that direction.

## Scope
Long-term exploration of the Linux kernel codebase with no fixed completion target. The objective is to see both the trees and the forest.

## Environment
VS Code, WSL2, Ubuntu Linux.
