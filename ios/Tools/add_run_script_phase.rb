#!/usr/bin/env ruby
# frozen_string_literal: true

# Adds a Run Script build phase to an Xcode target to de-duplicate shared core
# objects between libpscal_pascal_static.a and libpscal_rea_static.a before link.
#
# Usage:
#   gem install xcodeproj
#   ruby ios/Tools/add_run_script_phase.rb xcode/Pscal.xcodeproj pscal-runner
#
# Arguments:
#   ARGV[0] : Path to .xcodeproj (default: xcode/Pscal.xcodeproj)
#   ARGV[1] : Target name (default: pscal-runner)
#
# The added phase runs: bash "${PROJECT_DIR}/RunScriptPhase.sh"

require 'xcodeproj'

project_path = ARGV[0] || 'xcode/Pscal.xcodeproj'
target_name  = ARGV[1] || 'pscal-runner'

unless File.directory?(project_path)
  warn "error: project not found at #{project_path}"
  exit 1
end

project = Xcodeproj::Project.open(project_path)
target = project.targets.find { |t| t.name == target_name }

unless target
  warn "error: target '#{target_name}' not found in #{project_path}"
  warn "available targets: #{project.targets.map(&:name).join(', ')}"
  exit 2
end

phase_name = 'De-duplicate PSCAL core'
shell_script = 'bash "${PROJECT_DIR}/RunScriptPhase.sh"'

existing = target.shell_script_build_phases.find do |p|
  (p.name && p.name == phase_name) || (p.shell_script && p.shell_script.include?('RunScriptPhase.sh'))
end

if existing
  puts "[add_run_script_phase] Run Script already present on target '#{target_name}'."
else
  phase = target.new_shell_script_build_phase(phase_name)
  phase.shell_script = shell_script
  phase.show_env_vars_in_log = '1'

  # Reorder: place before the Frameworks phase if present, to ensure it runs before link.
  phases = target.build_phases
  fw_index = phases.index { |p| p.isa == 'PBXFrameworksBuildPhase' }
  if fw_index
    # Move the new phase to just before the Frameworks phase.
    target.build_phases.delete(phase)
    target.build_phases.insert(fw_index, phase)
  end

  puts "[add_run_script_phase] Added Run Script to target '#{target_name}'."
end

project.save
puts "[add_run_script_phase] Saved project: #{project_path}"
