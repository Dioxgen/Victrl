# Victrl — Authorization & Compliance Notes

> [中文](合规与授权说明.md) | English

## 1. Purpose of this document

Victrl is a **hardware AI agent**: it reads a target device's screen through an external HDMI capture card and drives the device through an emulated Bluetooth/USB keyboard and mouse. This document states plainly **when that is lawful, when it is not, and what the maintainers require of users**, so that neither users nor the project are exposed to unnecessary legal risk.

This document is an engineering-oriented risk note. **It is not legal advice.**

---

## 2. What Victrl does and does not do

| Victrl **does** | Victrl **does not** |
| --- | --- |
| Capture the target's HDMI/display output with an external capture card | Install, inject, hook, or modify **anything** on the target system |
| Emulate a standard Bluetooth/USB keyboard and mouse | Read the target's files, memory, disk, or internal data channels |
| Send screen frames plus task context to a multimodal model and execute the returned actions | Exploit a vulnerability, crack a credential, a certificate, or a signature |
| Operate **as the account that is already logged in** on the target | Bypass a password, a login screen, a permission policy, or a firewall |
| Keep task history, plans, and device profiles **on the Victrl host** | Hide itself from or disable the target's security software |

Two consequences matter legally:

1. **Victrl has no "break-in" capability.** It has no code whose purpose is to defeat a security protection measure. Under the prevailing reading of the 2011 judicial interpretation (Fa Shi [2011] No. 19, Art. 2), a tool is only a "program or tool specially used for intruding into or illegally controlling computer information systems" if it has the *function of evading or breaking through security protection measures* in order to obtain data or control **without authorization**. Victrl does not have that function.
2. **Victrl grants no authorization whatsoever.** It is a keyboard and a mouse. Whether using a keyboard on a given computer is lawful depends entirely on the relationship between the operator and the computer — never on the peripheral.

Therefore the decisive question is always: **whose device is it, and were you allowed to drive it?**

---

## 3. Legal framework (mainland China)

### 3.1 Article 285 of the Criminal Law

| Paragraph | Offence | Relevance to Victrl |
| --- | --- | --- |
| 285(1) | Illegal intrusion into computer information systems in the fields of **state affairs, national defence, or cutting-edge science and technology** | Applying Victrl to a classified or defence system is the clearest way to trigger this. This paragraph requires only the intrusion itself — no damage and no monetary threshold. |
| 285(2) | **Illegally obtaining computer information system data, or illegally controlling a computer information system** | This is the realistic risk. "Adopting other technical means" is an open-ended element that can cover HID/visual automation, and being staged *outside* the target does **not** place conduct outside this paragraph. No "break-in" is required: the statute reads "intrude ... **or adopt other technical means**". |
| 285(3) | Providing programs or tools specially used for intruding into or illegally controlling computer information systems | Aimed at the author/distributor. See §5. |

### 3.2 Thresholds ("serious circumstances") — Fa Shi [2011] No. 19, Article 1

Illegal control / illegal data acquisition reaches the criminal threshold when **any one** of the following is met:

- illegally **controlling 20 or more** computer information systems;
- obtaining **10 or more** sets of identity authentication information for online financial services (payment, securities, futures), or **500 or more** sets of other identity authentication information (accounts, passwords, digital certificates, ...);
- **illegal gains of CNY 5,000 or more**, or **economic loss of CNY 10,000 or more**;
- other serious circumstances.

These become **"especially serious"** at **five times** those thresholds (100 systems / 2,500 sets / CNY 25,000), which carries **3–7 years** imprisonment. Article 1 also provides that **knowingly using control over a system that another person has illegally controlled is punished under the same rules**.

*Practical reading:* a single authorized device used by its owner is nowhere near this threshold. Scale, lack of authorization, and profit drive these cases.

### 3.3 Other provisions worth knowing

- **Article 286** — destroying computer information systems (deleting, modifying, or adding data; or causing systems to malfunction). Deleting or altering another party's data through Victrl — even if you were allowed to operate the machine — can fall here. A parking-fee case in Guangzhou in which operators used remote-control software to delete fee records from a charging terminal was pursued under Article 286.
- **Cybersecurity Law, Article 27** — prohibits illegally intruding into another's network, interfering with its normal function, or stealing network data. This can apply even when the conduct does not reach the criminal threshold.
- **Contract and administrative rules** — the target device's software licence and an employer's IT/security policy may independently prohibit unattended automation, even on equipment you are allowed to use.

### 3.4 How courts have actually treated "no-install, human-like automation"

- **Guiding Case No. 145** (Supreme People's Court): obtaining server control by planting malware is "adopting other technical means" to illegally control a computer information system. It also confirms that control without **substantive functional destruction** is charged under Article 285(2) rather than 286.
- **China's first "AI cheat" case** (Yujiang District Court, Yingtan, Jiangxi; 6 May 2024): the "box" program *received mouse data commands transmitted through the computer's USB port, computed them, and sent the results back through the USB port* to move and click the mouse — i.e. hardware-level HID automation — and the seller was convicted of providing a tool
  specially used for intruding into or illegally controlling computer information systems, sentenced to 3 years (suspended) and fined.
- **The counter-arguments are real.** Practitioners have argued publicly that the AI-cheat program had no "evade or break through security measures" function and was in substance an automation tool, and that the judgment leaned on the harm to the game operator. Courts are not fully settled on where "human-like hardware automation" sits.

**What this means for Victrl.** The technical differences are favourable — Victrl is unidirectional (screen in, input out), installs nothing, reads no target data, breaks through nothing, and cannot act as an identity with more privilege than the logged-in user. But there is no formal immunity: if Victrl were positioned as, and sold for, controlling other people's computers without authorization, the "specially designed for" element could be argued, and a court that reasons from social harm rather than from the absence of a bypass could follow the AI-cheat case.

---

## 4. Use-case risk matrix

| Scenario | Article 285 exposure | Notes |
| --- | --- | --- |
| Your own PC, phone, lab machine, homelab | No | You hold the rights; no "unauthorized" element |
| Enterprise UI automation / test farm with the customer's written authorization | No | Keep the authorization on file; stay within its scope |
| Accessibility, legacy or industrial-HMI automation with the owner's consent | No | Consent should be documented; scope matters |
| A colleague's or family member's device with genuine, informed consent | Low, evidence-dependent | Be able to prove consent; do not exceed it |
| A company laptop you were issued but told not to automate | Low–moderate | Consent is limited by the employer's policy; Article 286 exposure if data is altered |
| Unattended use on someone else's machine without authorization | **Yes — 285(2)** | Thresholds above; 20+ systems / CNY 5,000 gain escalate quickly |
| Using Victrl to harvest credentials, passwords, or SMS/e-mail codes | **Yes — 285(2)** | The 10-set / 500-set thresholds are easy to cross |
| Deleting or altering another party's data through Victrl | **Also 286** | Damage offence; "you were allowed in" is not a defence to altering their data |
| Distributing/selling Victrl as a tool for controlling or breaking into others' computers | **Yes — 285(3)** | Targets the distribution conduct itself |

---

## 5. Rules the maintainers ask every user to follow

1. **Authorized targets only.** Connect Victrl only to (a) devices you own or lawfully possess, or (b) devices for which the owner or administrator has given **prior, documented authorization**. Keep that authorization in writing.
2. **No concealment.** Do not use Victrl to hide its presence or activity, and do not presenit as anything other than a keyboard/mouse peripheral.
3. **No unlawful purposes.** Do not use Victrl to obtain other people's credentials, authentication codes, or personal information, to commit fraud, or to deploy malware.
4. **Respect the target's rules.** Comply with the target device's software licence, the employer's IT policy, and any applicable security requirements.
5. **You are responsible.** The operator — not the project, and not the maintainers — bears responsibility for how Victrl is used. In mainland China, Articles 285 and 286 of the Criminal Law and Article 27 of the Cybersecurity Law can all apply to the operator, and distribution for unauthorized purposes can reach the distributor.

---

## 6. Engineering practices that reduce risk

If you build on Victrl, the following design choices both improve the product and document that it is not "specially designed" for unauthorized control:

- Record, at task start, **who authorized the run, for which device, and under what scope**; keep that record with the task log.
- Make the Victrl host's identity and activity **visible** — e.g. an on-screen indicator or a heartbeat on the target — rather than stealthy.
- Keep task logs and device profiles **on the Victrl host**, never on the target.
- Refuse to run **destructive** actions without an explicit, per-task confirmation.
- Do not add features whose purpose is to defeat, disable, or evade the target's security controls.
- Do not market or describe the project as a bypass, a crack, or a stealth tool.

---

## 7. Disclaimer

This document is a good-faith engineering notice, not legal advice and not a warranty of any kind. Project pages, roadmaps, and marketing copy are not legal opinions. If you intend to deploy Victrl commercially or in an enterprise environment, **obtain advice from a qualified lawyer in your jurisdiction** — in mainland China, a criminal practitioner with network-crime
experience.

The Apache 2.0 licence governs copyright permissions only. **It does not exempt anyone from criminal or administrative liability.**

*Last reviewed: 2026-05 · Victrl MVP (v2.1)*
