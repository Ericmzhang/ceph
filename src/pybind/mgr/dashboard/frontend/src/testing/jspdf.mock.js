class jsPDF {
  setFont() {}
  setFontSize() {}
  splitTextToSize() {
    return [];
  }
  text() {}
  addImage() {}
  addPage() {}
  save() {}
  internal = {
    pageSize: {
      getWidth: () => 210,
      getHeight: () => 297
    }
  };
}

module.exports = { jsPDF };
