// |reftest| skip-if(!xulRuntime.shell) -- bug xxx - fails to dismiss alert
/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is JavaScript Engine testing utilities.
 *
 * The Initial Developer of the Original Code is
 * Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2006
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s): Igor Bukanov
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

//-----------------------------------------------------------------------------
var BUGNUMBER = 341821;
var summary = 'Close hook crash';
var actual = 'No Crash';
var expect = 'No Crash';

var ialert = 0;

//-----------------------------------------------------------------------------
//test();
//-----------------------------------------------------------------------------

function test()
{
  enterFunc ('test');
  printBugNumber(BUGNUMBER);
  printStatus (summary);

  function generator()
  {
    try {
      yield [];
    } finally {
      make_iterator();
    }
  }

  function make_iterator()
  {
    var iter = generator();
    iter.next();
    iter = null;
    if (typeof alert != 'undefined')
    {
      alert(++ialert);
    }
  }

  make_iterator();

  // Trigger GC through the branch callback.
  for (var i = 0; i != 50000; ++i) {
    var x = {};
  }

  print('done');
  reportCompare(expect, actual, summary);

  exitFunc ('test');
}

function init()
{
  // give the dialog closer time to register
  setTimeout('runtest()', 5000);
}

function runtest()
{
  test();
  reportCompare(expect, actual, summary);
  gDelayTestDriverEnd = false;
  jsTestDriverEnd();
}

if (typeof window != 'undefined')
{
  // delay test driver end
  gDelayTestDriverEnd = true;

  window.addEventListener("load", init, false);
}
else
{
  reportCompare(expect, actual, summary);
}

