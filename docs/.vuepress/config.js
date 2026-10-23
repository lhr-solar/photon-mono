module.exports = {
  title: "Photon",
  description: "How to move around the page",
  base: "/photon-mono/",
  themeConfig: {
    sidebar: [
      {
        title: "Start",
        collapsable: false,
        children: [
          ["/", "Navigate"],
          ["/open", "Open the page"],
          ["/top-bar", "Top bar"],
          ["/logs", "Logs"],
        ],
      },
      {
        title: "Mechanical",
        collapsable: false,
        children: [
          ["/mechanical/", "Mechanical"],
          ["/mechanical/driver-input", "Driver Input"],
          ["/mechanical/chassis-imus", "Chassis IMUs"],
          ["/mechanical/suspension", "Suspension"],
          ["/mechanical/yaw", "Yaw"],
          ["/mechanical/wheels", "Wheels"],
          ["/mechanical/dynamics", "Dynamics"],
        ],
      },
      {
        title: "Working with data",
        collapsable: false,
        children: [
          ["/plots", "Plots"],
          ["/export", "Export a CSV"],
          ["/signals", "Signal names"],
        ],
      },
      {
        title: "Other tabs",
        collapsable: false,
        children: [
          ["/pit", "Pit"],
          ["/dashboard", "Dashboard"],
          ["/live-view", "Live View"],
          ["/pdu-and-can", "PDU and CAN"],
        ],
      },
      {
        title: "On track",
        collapsable: false,
        children: [
          ["/test-day", "Test day"],
        ],
      },
    ],
    displayAllHeaders: true,
    smoothScroll: true,
    searchPlaceholder: "Search...",
    repo: "lhr-solar/photon-mono",
    docsDir: "docs",
    repoLabel: "GitHub",
    editLinks: true,
    editLinkText: "Edit this page on GitHub",
    lastUpdated: "Last updated",
  },
}
