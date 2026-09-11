Any contribution that you make to this repository will
be under the Apache 2 License, as dictated by that
[license](http://www.apache.org/licenses/LICENSE-2.0.html):

~~~
5. Submission of Contributions. Unless You explicitly state otherwise,
   any Contribution intentionally submitted for inclusion in the Work
   by You to the Licensor shall be under the terms and conditions of
   this License, without any additional terms or conditions.
   Notwithstanding the above, nothing herein shall supersede or modify
   the terms of any separate license agreement you may have executed
   with Licensor regarding such Contributions.
~~~

Contributors must sign-off each commit by adding a `Signed-off-by: ...`
line to commit messages to certify that they have the right to submit
the code they are contributing to the project according to the
[Developer Certificate of Origin (DCO)](https://developercertificate.org/).

## Backports and ABI compatibility

Development happens on `rolling`; fixes reach the released distribution
branches (`lyrical`, `kilted`, `jazzy`, `humble`) through Mergify backports
after the PR is merged. Whether a fix is backported follows the ABI verdict
of the [abi workflow](.github/workflows/abi.yaml), per
[REP-0009](https://ros.org/reps/rep-0009.html):

| ABI verdict label | Backport labels set by CI | Result after merge |
| --- | --- | --- |
| `ABI compatible` | `backport-all` (unless `backport-<distro>` or `skip-backport` is already set) | backported to every supported distribution |
| `ABI break` | `skip-backport`, any `backport-*` removed | not backported; released branches must keep their ABI |
| none (diff could not be produced) | untouched | no automatic backport; a reviewer opts in with `backport-*` labels |

Reviewers can still narrow a compatible change to specific distributions by
setting `backport-<distro>` labels before the check completes, or block it
with `skip-backport`. A `skip-backport` set by a person is never removed by
CI. Mergify refuses to backport any PR that carries the `ABI break` label,
regardless of other labels.
