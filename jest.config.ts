// Copyright 2022 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

import { pathsToModuleNameMapper } from 'ts-jest';
import type { InitialOptionsTsJest } from 'ts-jest/dist/types';

const paths = {
  'pigweedjs/pw_*': ['./pw_*/ts'],
  'pigweedjs/protos/*': ['./dist/protos/*'],
};
const config: InitialOptionsTsJest = {
  preset: 'ts-jest/presets/js-with-ts',
  testRegex: '(/__tests__/.*|(\\_|/)(test|spec))\\.tsx?$',
  moduleNameMapper: pathsToModuleNameMapper(paths, { prefix: '<rootDir>/' }),
};

export default config;
