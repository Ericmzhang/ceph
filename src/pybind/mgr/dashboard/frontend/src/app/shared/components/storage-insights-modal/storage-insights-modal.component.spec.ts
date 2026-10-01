import { ComponentFixture, TestBed } from '@angular/core/testing';
import { HttpClientTestingModule } from '@angular/common/http/testing';
import { RouterTestingModule } from '@angular/router/testing';

import { configureTestBed } from '~/testing/unit-test-helper';
import { SharedModule } from '../../shared.module';
import { StorageInsightsModalComponent } from './storage-insights-modal.component';

describe('StorageInsightsModalComponent', () => {
  let component: StorageInsightsModalComponent;
  let fixture: ComponentFixture<StorageInsightsModalComponent>;

  configureTestBed({
    declarations: [StorageInsightsModalComponent],
    imports: [HttpClientTestingModule, RouterTestingModule, SharedModule]
  });

  beforeEach(() => {
    fixture = TestBed.createComponent(StorageInsightsModalComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });
});
